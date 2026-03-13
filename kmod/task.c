#include <linux/umh.h>
#include <uapi/linux/sched/types.h>

#include "internal.h"
#include "user.h"

static struct file *console_file = NULL;
static struct file *null_file = NULL;

void kstep_task_init(void) {
  console_file = filp_open("/dev/console", O_WRONLY, 0); // write only
  if (IS_ERR(console_file))
    panic("Failed to open /dev/console");
  null_file = filp_open("/dev/null", O_RDONLY, 0); // read only
  if (IS_ERR(null_file))
    panic("Failed to open /dev/null");
}

// Initialize stdin to `/dev/null` and stdout/stderr to `/dev/console`
// Reference: `console_on_rootfs` and `init_dup` in `init/main.c`
static int task_init(struct subprocess_info *info, struct cred *new) {
  const char *names[] = {"stdin", "stdout", "stderr"};
  struct file *files[] = {null_file, console_file, console_file};

  for (int i = 0; i < 3; i++) {
    int fd = get_unused_fd_flags(0);
    if (fd < 0 || fd != i)
      panic("get_unused_fd_flags returned %d for %s", fd, names[i]);
    fd_install(fd, get_file(files[i]));
  }

  *(struct task_struct **)info->data = current;
  TRACE_INFO("Task created with pid %d", current->pid);
  return 0;
}

struct task_struct *kstep_task_create(void) {
  char *path = "task";
  char *argv[] = {path, NULL, NULL};

  struct task_struct *p = NULL;
  struct subprocess_info *info = call_usermodehelper_setup(
      path, argv, NULL, GFP_KERNEL, task_init, NULL, &p);
  if (info == NULL)
    panic("Failed to setup user mode helper");

  if (call_usermodehelper_exec(info, UMH_WAIT_EXEC) < 0)
    panic("Failed to run user mode helper");

  if (p == NULL)
    panic("Failed to get task struct");

  // Wait for the task to start
  for (int i = 0; i < 100; i++) {
    kstep_sleep();
    if (strcmp(p->comm, TASK_READY_COMM) == 0) {
      kstep_task_kernel_pause(p);
      return p;
    }
    TRACE_INFO("Waiting for task %d to start", p->pid);
  }
  panic("Task %d did not start", p->pid);
}

static void kstep_task_signal(struct task_struct *p, enum sigcode code,
                              int val1, int val2, int val3) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      ._sifields = {._rt = {._sigval = {val1}, ._pid = val2, ._uid = val3}}};
  kstep_cov_enable_controller();
  send_sig_info(SIGUSR1, &info, p);
  kstep_cov_disable_controller();
  kstep_sleep();
}

void kstep_task_signal_fork(struct task_struct *p, int n) {
  kstep_task_signal(p, SIGCODE_FORK, n, 0, 0);
  TRACE_INFO("Forked task %d %d times", p->pid, n);
}

void kstep_task_signal_fifo(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_FIFO, 0, 0, 0);
  TRACE_INFO("Set task %d to FIFO", p->pid);
}

void kstep_task_signal_cfs(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_CFS, 0, 0, 0);
  TRACE_INFO("Set task %d to CFS", p->pid);
}

void kstep_task_signal_pin(struct task_struct *p, int begin, int end) {
  kstep_task_signal(p, SIGCODE_PIN, begin, end, 0);
  TRACE_INFO("Pinned task %d to CPUs %d-%d", p->pid, begin, end);
}

void kstep_task_signal_pause(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_PAUSE, 0, 0, 0);
  TRACE_INFO("Paused task %d", p->pid);
}

void kstep_task_signal_wakeup(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_WAKEUP, 0, 0, 0);
  TRACE_INFO("Waked up task %d", p->pid);
}

void kstep_task_signal_usleep(struct task_struct *p, int us) {
  kstep_task_signal(p, SIGCODE_USLEEP, us, 0, 0);
  TRACE_INFO("Put task %d to sleep for %d us", p->pid, us);
}

void kstep_task_signal_set_prio(struct task_struct *p, int prio) {
  kstep_task_signal(p, SIGCODE_SET_PRIO, prio, 0, 0);
  TRACE_INFO("Set priority of task %d to %d", p->pid, prio);
}

void kstep_task_kernel_set_prio(struct task_struct *p, int prio) {
  set_user_nice(p, prio);
  TRACE_INFO("Set priority of task %d to %d (kernel)", p->pid, prio);
}

void kstep_task_kernel_fifo(struct task_struct *p) {
  struct sched_attr attr = {
      .sched_policy = SCHED_FIFO,
      .sched_priority = 80,
  };
  sched_setattr_nocheck(p, &attr);
  TRACE_INFO("Set task %d to FIFO (kernel)", p->pid);
}

void kstep_task_kernel_cfs(struct task_struct *p) {
  struct sched_attr attr = {
      .sched_policy = SCHED_NORMAL,
      .sched_nice = 0,
  };
  sched_setattr_nocheck(p, &attr);
  TRACE_INFO("Set task %d to CFS (kernel)", p->pid);
}

void kstep_task_kernel_pin(struct task_struct *p, int begin, int end) {
  struct cpumask mask;
  cpumask_clear(&mask);
  for (int i = begin; i <= end; i++)
    cpumask_set_cpu(i, &mask);
  set_cpus_allowed_ptr(p, &mask);
  TRACE_INFO("Pinned task %d to CPUs %d-%d (kernel)", p->pid, begin, end);
}

void kstep_task_kernel_pause(struct task_struct *p) {
  // Set the task to TASK_UNINTERRUPTIBLE so that when __schedule() runs on
  // the task's CPU, it will take the blocking path and properly dequeue
  // the task locally. Using TASK_UNINTERRUPTIBLE avoids the signal_pending
  // check in try_to_block_task() that would abort the block.
  WRITE_ONCE(p->__state, TASK_UNINTERRUPTIBLE);

  // Trigger a reschedule on the task's CPU. When returning to userspace,
  // exit_to_user_mode_loop() calls schedule() (non-preempt), which will see
  // the non-RUNNING state and dequeue the task.
  set_tsk_thread_flag(p, TIF_NEED_RESCHED);
  kick_process(p);

  kstep_sleep();
  TRACE_INFO("Paused task %d (kernel)", p->pid);
}

void kstep_task_kernel_wakeup(struct task_struct *p) {
  wake_up_process(p);
  TRACE_INFO("Waked up task %d (kernel)", p->pid);
}

// Not possible as kernel counterparts:
// - kstep_task_kernel_fork: forking requires userspace fork() syscall
// - kstep_task_kernel_usleep: requires task to call usleep() in userspace

