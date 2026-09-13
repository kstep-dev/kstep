#include <linux/anon_inodes.h>
#include <linux/umh.h>
#include <linux/xarray.h>
#include <uapi/linux/sched/types.h>
#include <linux/spinlock.h>

#include "internal.h"
#include "user.h"

// Written by the task itself only, so no two writers race; the controller reads it.
enum kstep_task_state {
  KSTEP_TASK_NEW,     // created, not yet at its first control-file read, which parks it
  KSTEP_TASK_ACTIVE,  // between halts: acting on a command, or asleep in a syscall (pause, wait)
  KSTEP_TASK_SETTLED, // in the halt of kstep_ctrl_read(): the controller may look at its CPU (see
                      // kstep_settle). To the scheduler a CPU-bound task, rq->curr consuming its
                      // slice; the halt only lets the CPU sleep until the next tick instead of spin.
};

// kSTEP's record of every task that uses the control file: created before a created task's first
// instruction (task_init), on a fork child's first read, or by the controller's first command to
// it; never removed. Keyed by pid (a dense index, as an xarray wants; kSTEP's tasks are
// single-threaded processes) rather than by file, which fork children share with their parent.
// The lookup is the hot one (every halt return, every settle poll, the migrate hook with the
// runqueue locked) and lock-free; the lock makes find-or-create atomic, so a task's first read
// and a controller command racing for it create one record.
struct kstep_task {
  struct task_struct *p; // referenced: stays readable (exit_state) after the task exits
  enum kstep_task_state state;
  struct completion parked; // the first read has parked the task (see kstep_task_create)
  struct kstep_msg msg; // cmd is KSTEP_CMD_NONE when empty
};
static DEFINE_XARRAY(kstep_tasks);
static DEFINE_SPINLOCK(kstep_tasks_lock);

static struct kstep_task *kstep_task_find(struct task_struct *p) {
  struct kstep_task *t = xa_load(&kstep_tasks, p->pid);
  return t && t->p == p ? t : NULL; // a reused pid does not find the dead task's record
}

// The record of p, created in `state` if there is none yet.
static struct kstep_task *kstep_task_get(struct task_struct *p, enum kstep_task_state state) {
  struct kstep_task *t;
  unsigned long flags;

  spin_lock_irqsave(&kstep_tasks_lock, flags);
  t = kstep_task_find(p);
  if (!t) {
    t = kzalloc(sizeof(*t), GFP_ATOMIC);
    if (!t)
      panic("Failed to record task %d", p->pid);
    t->p = get_task_struct(p);
    t->state = state;
    init_completion(&t->parked);
    // initialized before it is published: lookups take no lock
    if (xa_err(xa_store(&kstep_tasks, p->pid, t, GFP_ATOMIC)))
      panic("Failed to record task %d", p->pid);
  }
  spin_unlock_irqrestore(&kstep_tasks_lock, flags);
  return t;
}

// Queue a command; the task acts on it when it next returns from its halt. Nothing here
// touches the scheduler: no signal, no IPI.
static void kstep_task_send(struct task_struct *p, int cmd, int arg) {
  struct kstep_task *t = kstep_task_get(p, KSTEP_TASK_ACTIVE);
  if (READ_ONCE(t->msg.cmd) != KSTEP_CMD_NONE)
    panic("Task %d has not picked up command %d yet", p->pid, t->msg.cmd);
  t->msg.arg = arg;
  smp_store_release(&t->msg.cmd, cmd);
}

bool kstep_task_settled(struct task_struct *p) {
  struct kstep_task *t = kstep_task_find(p);
  return t && READ_ONCE(t->state) == KSTEP_TASK_SETTLED && !signal_pending(p);
}

static ssize_t kstep_ctrl_read(struct file *f, char __user *buf, size_t len, loff_t *off) {
  struct kstep_task *t = kstep_task_get(current, KSTEP_TASK_ACTIVE);

  // A new task parks on its first read (TASK_INTERRUPTIBLE, like pause()) until its first
  // wakeup, and tells kstep_task_create() so. The controller shares this CPU and wakes on
  // complete(), but runs only once this task has switched out: no preemption point lies between
  // the two calls (CONFIG_PREEMPT_NONE, see linux/config.kstep). So no spin and no polling.
  if (t->state == KSTEP_TASK_NEW) {
    t->state = KSTEP_TASK_ACTIVE;
    set_current_state(TASK_INTERRUPTIBLE); // before complete(): parked once the controller looks
    complete(&t->parked);
    schedule(); // until the first wakeup; nothing else wakes a parked task
    return 0;
  }

  // A queued command is handed over instead of halting.
  if (smp_load_acquire(&t->msg.cmd) != KSTEP_CMD_NONE) {
    struct kstep_msg msg = t->msg;
    WRITE_ONCE(t->msg.cmd, KSTEP_CMD_NONE);
    if (len < sizeof(msg) || copy_to_user(buf, &msg, sizeof(msg)))
      panic("Task %d cannot receive command %d", current->pid, msg.cmd);
    TRACE_INFO("Task %d picks up command %d (%d)", current->pid, msg.cmd, msg.arg);
    return sizeof(msg);
  }

  // Check and halt with interrupts off, so an interrupt in between wakes the halt, not lost.
  local_irq_disable();
  if (!need_resched() && !signal_pending(current)) {
    WRITE_ONCE(t->state, KSTEP_TASK_SETTLED);
#if defined(CONFIG_X86)
    arch_safe_halt(); // sti; hlt (or the paravirt op): interrupts come on only as it halts
#elif defined(CONFIG_ARM64)
    wfi(); // wakes on a pending interrupt even while masked
#else
#error "no halt for this architecture"
#endif
    WRITE_ONCE(t->state, KSTEP_TASK_ACTIVE);
  }
  local_irq_enable(); // a no-op after arch_safe_halt
  return 0; // back to user mode, where a pending reschedule or signal is acted on
}

static const struct file_operations kstep_ctrl_fops = {.read = kstep_ctrl_read};

// One open file each, shared by every task (like fds inherited across fork): none has state.
static struct file *console_file = NULL;
static struct file *null_file = NULL;
static struct file *ctrl_file = NULL;

void kstep_task_init(void) {
  console_file = filp_open("/dev/console", O_WRONLY, 0); // write only
  if (IS_ERR(console_file))
    panic("Failed to open /dev/console");
  null_file = filp_open("/dev/null", O_RDONLY, 0); // read only
  if (IS_ERR(null_file))
    panic("Failed to open /dev/null");
  ctrl_file = anon_inode_getfile("kstep", &kstep_ctrl_fops, NULL, O_RDONLY);
  if (IS_ERR(ctrl_file))
    panic("Failed to create the control file");
}

// Initialize stdin to `/dev/null`, stdout/stderr to `/dev/console`, and KSTEP_CTRL_FD to the
// control file. Reference: `console_on_rootfs` and `init_dup` in `init/main.c`
static int task_init(struct subprocess_info *info, struct cred *new) {
  const char *names[] = {"stdin", "stdout", "stderr", "kstep"};
  struct file *files[] = {null_file, console_file, console_file, ctrl_file};

  for (int i = 0; i < ARRAY_SIZE(files); i++) {
    int fd = get_unused_fd_flags(0);
    if (fd < 0 || fd != i)
      panic("get_unused_fd_flags returned %d for %s", fd, names[i]);
    fd_install(fd, get_file(files[i]));
  }

  // Start on the controller's CPU: it runs whenever the controller blocks, whatever the other
  // CPUs are doing. kstep_task_create() moves it once parked.
  if (set_cpus_allowed_ptr(current, cpumask_of(0)))
    panic("Failed to pin task %d to CPU 0", current->pid);

  kstep_task_get(current, KSTEP_TASK_NEW); // the first read parks
  *(struct task_struct **)info->data = current;
  TRACE_INFO("Task created with pid %d", current->pid);
  return 0;
}

struct task_struct *kstep_task_create(void) {
  char *argv[] = {"task", NULL};

  struct task_struct *p = NULL;
  struct subprocess_info *info = call_usermodehelper_setup(
      "/user", argv, NULL, GFP_KERNEL, task_init, NULL, &p);
  if (info == NULL)
    panic("Failed to setup user mode helper");

  if (call_usermodehelper_exec(info, UMH_WAIT_EXEC) < 0)
    panic("Failed to run user mode helper");

  if (p == NULL)
    panic("Failed to get task struct");

  // Parked: off CPU 0 in TASK_INTERRUPTIBLE (see kstep_ctrl_read).
  wait_for_completion(&kstep_task_find(p)->parked);
  TRACE_INFO("Task %d is ready", p->pid);
  kstep_task_pin(p, 1, num_online_cpus() - 1);
  kstep_reset_task(p);
  return p;
}

static void kstep_task_signal(struct task_struct *p, enum sigcode code,
                              int val) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1, .si_code = code, .si_int = val};
  kstep_cov_enable_controller();
  send_sig_info(SIGUSR1, &info, p);
  kstep_cov_disable_controller();
  kstep_settle();
  // Still pending: the task is not current (it acts after a tick) or frozen. Fine for a wakeup,
  // whose effect is the wakeup itself; settle covers it (rq->ttwu_pending is set before the IPI).
  if (code != SIGCODE_WAKEUP && signal_pending(p))
    TRACE_INFO("Task %d has not handled signal code %d yet", p->pid, code);
}

// The forks happen once the task next runs (after the next tick), in its own context.
void kstep_task_fork(struct task_struct *p, int n) {
  kstep_task_send(p, KSTEP_CMD_FORK, n);
  TRACE_INFO("Task %d will fork %d times", p->pid, n);
}

void kstep_task_pause(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_PAUSE, 0);
  TRACE_INFO("Paused task %d", p->pid);
}

// Ask the task to exit (it does so the next time it runs and handles the signal).
void kstep_task_exit(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_EXIT, 0);
  TRACE_INFO("Exiting task %d", p->pid);
}

// wait/post: a counting semaphore shared by all tasks, implemented as one pipe (the FIFO
// PIPE_PATH, created by init before the module loads) whose bytes are the tokens. wait sleeps in pipe_read() (TASK_INTERRUPTIBLE on the pipe's wait queue)
// until a token is there and consumes it; post writes a token, and pipe_write() wakes one
// waiter through wake_up_interruptible_sync_poll(), i.e. a WF_SYNC wakeup issued from the
// poster's CPU: the production sync-wakeup path. A post with no waiter leaves a token, so the
// next wait returns at once (a poster that should sleep uses kstep_task_pause).
void kstep_task_wait(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_WAIT, 0);
  TRACE_INFO("Task %d waits", p->pid);
}

void kstep_task_post(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_POST, 0);
  TRACE_INFO("Task %d posts (sync wakeup of one waiter)", p->pid);
}

void kstep_task_wakeup(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_WAKEUP, 0);
  TRACE_INFO("Waked up task %d", p->pid);
}

// Block task via nanosleep, which enters do_nanosleep() and sets
// TASK_INTERRUPTIBLE | TASK_FREEZABLE. Unlike kstep_task_pause (which uses
// pause() and only sets TASK_INTERRUPTIBLE), this makes the task freezable.
void kstep_task_block(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_BLOCK, 0);
  TRACE_INFO("Blocked task %d", p->pid);
}

// nice applies to the fair classes only; the kernel keeps it across a spell as fifo/rr.
void kstep_task_set_nice(struct task_struct *p, int nice) {
  kstep_cov_enable_controller();
  set_user_nice(p, nice);
  kstep_cov_disable_controller();
  TRACE_INFO("Set nice of task %d to %d", p->pid, nice);
}

// Scheduling class: SCHED_NORMAL, SCHED_BATCH, SCHED_IDLE, SCHED_FIFO or SCHED_RR. The fair
// classes keep the task's nice; the real-time ones get one fixed priority, since priority only
// orders real-time tasks among themselves and kSTEP studies their effect on the fair class.
void kstep_task_set_policy(struct task_struct *p, int policy) {
  struct sched_attr attr = {.sched_policy = policy};

  if (policy == SCHED_FIFO || policy == SCHED_RR)
    attr.sched_priority = 80;
  else
    attr.sched_nice = task_nice(p);
  kstep_cov_enable_controller();
  sched_setattr_nocheck(p, &attr);
  kstep_cov_disable_controller();
  TRACE_INFO("Set policy of task %d to %d", p->pid, policy);
}

// Returns -EINVAL when the mask does not intersect the task's cpuset (cgroup cpuset.cpus).
int kstep_task_set_affinity(struct task_struct *p, const struct cpumask *mask) {
  // directly call set_cpus_allowed_ptr is not enough, as it does not update the user_cpus_ptr
  KSYM_IMPORT(sched_setaffinity);
  kstep_cov_enable_controller();
  int err = KSYM_sched_setaffinity(p->pid, mask);
  kstep_cov_disable_controller();
  return err;
}

void kstep_task_pin(struct task_struct *p, int begin, int end) {
  struct cpumask mask;
  cpumask_clear(&mask);
  for (int i = begin; i <= end; i++)
    cpumask_set_cpu(i, &mask);
  if (kstep_task_set_affinity(p, &mask))
    panic("Failed to set CPU affinity for task %d to CPUs %*pbl", p->pid, cpumask_pr_args(&mask));
  TRACE_INFO("Pinned task %d to CPUs %d-%d", p->pid, begin, end);
}
