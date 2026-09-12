#include <linux/anon_inodes.h>
#include <linux/umh.h>
#include <uapi/linux/sched/types.h>
#include <linux/spinlock.h>

#include "internal.h"
#include "user.h"

// The task halting in kstep_ctrl_read() on each CPU, NULL otherwise. Written by that task only, with
// interrupts off; kstep_settle() compares it with rq->curr, so a stale value never passes for another task.
DEFINE_PER_CPU(struct task_struct *, kstep_settled_task);

static ssize_t kstep_ctrl_read(struct file *f, char __user *buf, size_t len, loff_t *off) {
  // Check and halt with interrupts off, so an interrupt in between wakes the halt, not lost.
  local_irq_disable();
  if (!need_resched() && !signal_pending(current)) {
    __this_cpu_write(kstep_settled_task, current);
#if defined(CONFIG_X86)
    arch_safe_halt(); // sti; hlt (or the paravirt op): interrupts come on only as it halts
#elif defined(CONFIG_ARM64)
    wfi(); // wakes on a pending interrupt even while masked
#else
#error "no halt for this architecture"
#endif
    __this_cpu_write(kstep_settled_task, NULL);
  }
  local_irq_enable(); // a no-op after arch_safe_halt
  return 0; // back to user mode, where a pending reschedule or signal is acted on
}

// A new task parks here (TASK_INTERRUPTIBLE, like pause()) until its first wakeup, and tells
// kstep_task_create() so. The controller shares this CPU and wakes on complete(), but runs only
// once this task has switched out: no preemption point lies between the two calls
// (CONFIG_PREEMPT_NONE, see linux/config.kstep). So no spin and no polling on either side.
static DECLARE_COMPLETION(kstep_task_parked);

static ssize_t kstep_ctrl_write(struct file *f, const char __user *buf, size_t len, loff_t *off) {
  set_current_state(TASK_INTERRUPTIBLE); // before complete(): parked once the controller looks
  complete(&kstep_task_parked);
  schedule(); // until the first wakeup; nothing else wakes a parked task
  return 0;
}

static const struct file_operations kstep_ctrl_fops = {.read = kstep_ctrl_read,
                                                       .write = kstep_ctrl_write};

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
  ctrl_file = anon_inode_getfile("kstep", &kstep_ctrl_fops, NULL, O_RDWR);
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

  *(struct task_struct **)info->data = current;
  TRACE_INFO("Task created with pid %d", current->pid);
  return 0;
}

struct task_struct *kstep_task_create(void) {
  char *argv[] = {"task", NULL};

  struct task_struct *p = NULL;
  reinit_completion(&kstep_task_parked);
  struct subprocess_info *info = call_usermodehelper_setup(
      "/user", argv, NULL, GFP_KERNEL, task_init, NULL, &p);
  if (info == NULL)
    panic("Failed to setup user mode helper");

  if (call_usermodehelper_exec(info, UMH_WAIT_EXEC) < 0)
    panic("Failed to run user mode helper");

  if (p == NULL)
    panic("Failed to get task struct");

  // Parked: off CPU 0 in TASK_INTERRUPTIBLE (see kstep_ctrl_write).
  wait_for_completion(&kstep_task_parked);
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

void kstep_task_fork(struct task_struct *p, int n) {
  kstep_task_signal(p, SIGCODE_FORK, n);
  TRACE_INFO("Forked task %d %d times", p->pid, n);
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
#define KSTEP_RT_PRIORITY 80
void kstep_task_set_policy(struct task_struct *p, int policy) {
  struct sched_attr attr = {.sched_policy = policy};

  if (policy == SCHED_FIFO || policy == SCHED_RR)
    attr.sched_priority = KSTEP_RT_PRIORITY;
  else
    attr.sched_nice = task_nice(p);
  kstep_cov_enable_controller();
  sched_setattr_nocheck(p, &attr);
  kstep_cov_disable_controller();
  TRACE_INFO("Set policy of task %d to %d", p->pid, policy);
}

void kstep_task_set_affinity(struct task_struct *p, const struct cpumask *mask) {
  // directly call set_cpus_allowed_ptr is not enough, as it does not update the user_cpus_ptr
  KSYM_IMPORT(sched_setaffinity);
  kstep_cov_enable_controller();
  if (KSYM_sched_setaffinity(p->pid, mask)) {
    kstep_cov_disable_controller();
    panic("Failed to set CPU affinity for task %d to CPUs %*pbl", p->pid, cpumask_pr_args(mask));
  }
  kstep_cov_disable_controller();
}

void kstep_task_pin(struct task_struct *p, int begin, int end) {
  struct cpumask mask;
  cpumask_clear(&mask);
  for (int i = begin; i <= end; i++)
    cpumask_set_cpu(i, &mask);
  kstep_task_set_affinity(p, &mask);
  TRACE_INFO("Pinned task %d to CPUs %d-%d", p->pid, begin, end);
}
