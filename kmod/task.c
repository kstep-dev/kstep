#include <linux/anon_inodes.h>
#include <linux/umh.h>
#include <linux/xarray.h>
#include <uapi/linux/sched/types.h>

#include "internal.h"
#include "user.h"

#define KSTEP_CTRL_QUEUE 8 // deeper than any driver asks for between two reads; full is a bug

// kSTEP's record of every task that uses the control file, created before the task's first
// instruction (task_init) and never removed, so every later lookup only ever finds one. Keyed by
// pid, a dense index as an xarray wants; kSTEP's tasks are single-threaded processes. The lookup
// is the hot one (every halt return, every settle poll, the migrate hook with the runqueue
// locked) and needs no lock: the only writer is the task's own creation, before it can run.
struct kstep_task {
  struct task_struct *p; // referenced: stays readable (exit_state) after the task exits
  // In the halt of kstep_ctrl_read(), where the controller may look at its CPU (kstep_settle).
  // Written by the task itself only; the controller reads it.
  bool settled;
  // Asked for, not yet taken, oldest first. A queue, not one slot: two posts are two tokens, and
  // a kill behind a wait must not swallow it. The two ends never run at once.
  int queue[KSTEP_CTRL_QUEUE];
  u32 head, tail;
};
static DEFINE_XARRAY(kstep_tasks);

static struct kstep_task *kstep_task_find(struct task_struct *p) {
  struct kstep_task *t = xa_load(&kstep_tasks, p->pid);
  return t && t->p == p ? t : NULL; // a reused pid does not find the dead task's record
}

// Take the oldest action, or NONE. Only the task itself calls this, for itself.
static int kstep_ctrl_take(struct kstep_task *t) {
  if (t->head == t->tail)
    return KSTEP_CTRL_NONE;
  return t->queue[t->head++ % KSTEP_CTRL_QUEUE];
}

// Add one. Only the controller calls this, between steps.
static void kstep_ctrl_put(struct kstep_task *t, enum kstep_ctrl act) {
  if (t->tail - t->head >= KSTEP_CTRL_QUEUE)
    panic("Task %d has more than %d actions outstanding", t->p->pid, KSTEP_CTRL_QUEUE);
  t->queue[t->tail++ % KSTEP_CTRL_QUEUE] = act;
}

// Record p, which has no record yet. Runs in the new task's own context, before it runs.
static void kstep_task_add(struct task_struct *p) {
  struct kstep_task *t = kzalloc(sizeof(*t), GFP_KERNEL);

  if (!t)
    panic("Failed to record task %d", p->pid);
  t->p = get_task_struct(p);
  // A new task parks on its first read (see kstep_ctrl_read): no state for "created but not yet
  // there", because the queue already says what it will do when it gets there.
  kstep_ctrl_put(t, KSTEP_CTRL_PARK);
  // initialized before it is published: lookups take no lock
  if (xa_err(xa_store(&kstep_tasks, p->pid, t, GFP_KERNEL)))
    panic("Failed to record task %d", p->pid);
}

bool kstep_task_settled(struct task_struct *p) {
  struct kstep_task *t = kstep_task_find(p);
  // Settled means there is nothing left for this task to do: it is in the halt, no action is
  // waiting in its queue, and no signal is pending. The queue matters as much as the halt -- an
  // action is queued while the task is still halted, so without this the controller could see the
  // CPU as settled and move on before the task had performed what it was asked. That is what
  // !signal_pending() used to cover, when an ask was a signal.
  return t && READ_ONCE(t->settled) && t->head == t->tail && !signal_pending(p);
}

// A new task parks here (TASK_INTERRUPTIBLE, like pause()) until its first wakeup, and tells
// kstep_task_create() so. The controller shares this CPU and wakes on complete(), but runs only
// once this task has switched out: no preemption point lies between the two calls
// (CONFIG_PREEMPT_NONE, see linux/config.kstep). So no spin and no polling on either side, and
// nothing depends on which of the two the scheduler would pick: with the clock frozen, CFS
// (before 6.6) picks the controller for as long as it yields.
static DECLARE_COMPLETION(kstep_task_parked);

static ssize_t kstep_ctrl_read(struct file *f, char __user *buf, size_t len, loff_t *off) {
  struct kstep_task *t = kstep_task_find(current);

  if (!t)
    panic("Task %d has no kSTEP record", current->pid);

  int act = kstep_ctrl_take(t);

  if (act == KSTEP_CTRL_PARK) {
    set_current_state(TASK_INTERRUPTIBLE); // before complete(): parked once the controller looks
    complete(&kstep_task_parked);
    schedule(); // until the first wakeup (SIGUSR1); nothing else wakes a parked task
    return 0;
  }

  // An action is the task's own to do: hand it over and let it make the syscall.
  if (act != KSTEP_CTRL_NONE) {
    char c = act;

    if (!len || copy_to_user(buf, &c, 1))
      panic("Task %d cannot be given action %d", current->pid, act);
    return 1;
  }

  // Check and halt with interrupts off, so an interrupt in between wakes the halt, not lost.
  local_irq_disable();
  if (!need_resched() && !signal_pending(current)) {
    WRITE_ONCE(t->settled, true);
#if defined(CONFIG_X86)
    arch_safe_halt(); // sti; hlt (or the paravirt op): interrupts come on only as it halts
#elif defined(CONFIG_ARM64)
    wfi(); // wakes on a pending interrupt even while masked
#else
#error "no halt for this architecture"
#endif
    WRITE_ONCE(t->settled, false);
  }
  local_irq_enable(); // a no-op after arch_safe_halt
  return 0; // back to user mode, where a pending reschedule or signal is acted on
}

static const struct file_operations kstep_ctrl_fops = {.read = kstep_ctrl_read};

// One open file each, shared by every task: none has state.
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
  // CPUs are doing. kstep_task_create() moves it once it is asleep.
  if (set_cpus_allowed_ptr(current, cpumask_of(0)))
    panic("Failed to pin task %d to CPU 0", current->pid);

  kstep_task_add(current); // the first read says pause
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

  // The task does its own init and its first read parks it (kstep_task_add, kstep_ctrl_read):
  // asleep, and off this CPU by the time the controller runs again.
  wait_for_completion(&kstep_task_parked);
  TRACE_INFO("Task %d is ready", p->pid);
  char cpus[16];
  snprintf(cpus, sizeof(cpus), "1-%d", num_online_cpus() - 1);
  if (kstep_task_set_affinity(p, cpus))
    panic("Failed to set CPU affinity for task %d to CPUs %s", p->pid, cpus);
  kstep_reset_task(p);
  return p;
}

static void kstep_nop(void *unused) {}

// Queue an action for p's next control-file read. Queuing is all this does: a task halted at the
// read is let out of it; anywhere else it acts when it next reaches a read, and a sleeping task
// reaches none until a driver wakes it. The IPI is the tick's own call, which sets no reschedule
// and so moves nothing unasked. Waiting for the action to have happened is the caller's settle.
static void kstep_task_ctrl(struct task_struct *p, enum kstep_ctrl act) {
  struct kstep_task *t = kstep_task_find(p);

  if (!t)
    panic("Task %d has no kSTEP record", p->pid);

  kstep_ctrl_put(t, act);
  // A halted task is at the read already and only has to be let out of the halt. The IPI is the
  // tick's own call: it sets no reschedule, so it moves nothing that was not asked for.
  if (READ_ONCE(t->settled)) {
    kstep_cov_controller(true);
    smp_call_function_single(task_cpu(p), kstep_nop, NULL, 1);
    kstep_cov_controller(false);
  }
  // Settle either way, as every verb did when an ask was a signal: the ask is done when the task
  // has performed it, and the driver's next line is written expecting that. A task asleep
  // elsewhere never reaches the read, and for that one the settle returns once its CPU is idle.
  kstep_settle();
}

void kstep_task_pause(struct task_struct *p) {
  kstep_task_ctrl(p, KSTEP_CTRL_PAUSE);
  TRACE_INFO("Paused task %d", p->pid);
}

void kstep_task_exit(struct task_struct *p) {
  kstep_task_ctrl(p, KSTEP_CTRL_EXIT);
  TRACE_INFO("Exiting task %d", p->pid);
}

void kstep_task_chan_read(struct task_struct *p) {
  kstep_task_ctrl(p, KSTEP_CTRL_CHAN_READ);
  TRACE_INFO("Task %d waits", p->pid);
}

void kstep_task_chan_write(struct task_struct *p) {
  kstep_task_ctrl(p, KSTEP_CTRL_CHAN_WRITE);
  TRACE_INFO("Task %d posts (sync wakeup of one waiter)", p->pid);
}

// Nothing to queue: the point is to return the task from wherever it is asleep, which only a
// signal does. The handler does nothing; the interruption is the whole of it. As with an action,
// the caller's settle is what waits for the task to have taken it (kstep_task_settled).
void kstep_task_wakeup(struct task_struct *p) {
  kstep_cov_controller(true);
  send_sig(SIGUSR1, p, 0); // no siginfo to fill: the signal says nothing
  kstep_cov_controller(false);
  kstep_settle(); // the ask is done when the task is running again, not when the signal is sent
  TRACE_INFO("Waked up task %d", p->pid);
}

// Block task via nanosleep, which enters do_nanosleep() and sets
// TASK_INTERRUPTIBLE | TASK_FREEZABLE. Unlike kstep_task_pause (which uses
// pause() and only sets TASK_INTERRUPTIBLE), this makes the task freezable.
void kstep_task_block(struct task_struct *p) {
  kstep_task_ctrl(p, KSTEP_CTRL_BLOCK);
  TRACE_INFO("Blocked task %d", p->pid);
}

// A fair policy -- SCHED_NORMAL, SCHED_BATCH or SCHED_IDLE -- and the nice that goes with it,
// set together as sched_setattr does. SCHED_IDLE ignores the nice and runs at the class's fixed
// minimal weight.
void kstep_task_set_fair(struct task_struct *p, int policy, int nice) {
  struct sched_attr attr = {.sched_policy = policy, .sched_nice = nice};

  kstep_cov_controller(true);
  sched_setattr_nocheck(p, &attr);
  kstep_cov_controller(false);
  TRACE_INFO("Set policy of task %d to %d (nice %d)", p->pid, policy, nice);
}

// A real-time policy -- SCHED_FIFO or SCHED_RR -- and the priority that goes with it, 1..99. The
// kernel takes the two together and has no default priority, and neither does kSTEP. The task's
// nice is kept meanwhile, inert.
void kstep_task_set_rt(struct task_struct *p, int policy, int prio) {
  struct sched_attr attr = {.sched_policy = policy, .sched_priority = prio};

  kstep_cov_controller(true);
  sched_setattr_nocheck(p, &attr);
  kstep_cov_controller(false);
  TRACE_INFO("Set policy of task %d to %d (priority %d)", p->pid, policy, prio);
}

// Restrict p to the CPUs of cpulist, such as "1", "1-3" or "1,3": test CPUs only, never CPU 0,
// the controller's. Returns -ERANGE for a list that is not that, and -EINVAL when the list does
// not intersect the task's cpuset (cgroup cpuset.cpus), as sched_setaffinity does.
int kstep_task_set_affinity(struct task_struct *p, const char *cpulist) {
  // directly call set_cpus_allowed_ptr is not enough, as it does not update the user_cpus_ptr
  KSYM_IMPORT(sched_setaffinity);
  struct cpumask mask;

  if (!kstep_parse_cpus(cpulist, &mask))
    return -ERANGE;
  kstep_cov_controller(true);
  int ret = KSYM_sched_setaffinity(p->pid, &mask);
  kstep_cov_controller(false);
  TRACE_INFO("Set affinity of task %d to CPUs %s (%d)", p->pid, cpulist, ret);
  return ret;
}
