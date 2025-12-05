#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/umh.h>

#include "kstep.h"

struct task_struct *busy_task = NULL;

// Initialize stdin, stdout, and stderr to /dev/console
// Reference: `console_on_rootfs` and `init_dup` in `init/main.c`
static void init_task_console(void) {
  struct file *console_file = filp_open("/dev/console", O_RDWR, 0);
  if (IS_ERR(console_file))
    panic("Failed to open /dev/console");

  for (int i = 0; i < 3; i++) { // stdin, stdout, stderr
    int fd = get_unused_fd_flags(0);
    if (fd < 0 || fd != i)
      panic("get_unused_fd_flags returned %d for fd %d", fd, i);
    fd_install(fd, get_file(console_file));
  }
  fput(console_file);
}

static int busy_task_init(struct subprocess_info *info, struct cred *new) {
  init_task_console();
  busy_task = current;
  TRACE_INFO("busy task created with pid %d", busy_task->pid);
  return 0;
}

void kstep_tasks_init(void) {
  char *path = "/busy";
  char *argv[] = {path, NULL};
  struct subprocess_info *info = call_usermodehelper_setup(
      path, argv, NULL, GFP_KERNEL, busy_task_init, NULL, NULL);
  if (info == NULL)
    panic("Failed to setup user mode helper");

  if (call_usermodehelper_exec(info, UMH_WAIT_EXEC) < 0)
    panic("Failed to run user mode helper");

  for (int i = 0; i < 100; i++) {
    if (strcmp(busy_task->comm, "test-proc") == 0)
      return;
    kstep_sleep();
    TRACE_INFO("Waiting for busy task to start");
  }
  panic("Busy task did not start");
}

void kstep_task_signal(struct task_struct *p, enum sigcode code, int val1,
                       int val2, int val3) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      ._sifields = {._rt = {._sigval = {val1}, ._pid = val2, ._uid = val3}}};
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (val1=%d, val2=%d, val3=%d) to pid %d",
             sigcode_to_str[code], val1, val2, val3, p->pid);
  kstep_sleep();
}

void kstep_task_fork(struct task_struct *p, int n) {
  kstep_task_signal(p, SIGCODE_FORK, n, 0, 0);
  TRACE_INFO("Forked task %d %d times", p->pid, n);
}

void kstep_task_fork_pin(struct task_struct *p, int n, int begin, int end) {
  kstep_task_signal(p, SIGCODE_FORK_PIN, n, begin, end);
  TRACE_INFO("Forked task %d %d times and pinned to CPUs %d-%d", p->pid, n,
             begin, end);
}

void kstep_task_pin(struct task_struct *p, int begin, int end) {
  kstep_task_signal(p, SIGCODE_PIN, begin, end, 0);
  TRACE_INFO("Pinned task %d to CPUs %d-%d", p->pid, begin, end);
}

void kstep_task_pause(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_PAUSE, 0, 0, 0);
  TRACE_INFO("Paused task %d", p->pid);
}

void kstep_task_wakeup(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_WAKEUP, 0, 0, 0);
  TRACE_INFO("Waked up task %d", p->pid);
}

void kstep_task_sleep(struct task_struct *p, int n) {
  kstep_task_signal(p, SIGCODE_SLEEP, n, 0, 0);
  TRACE_INFO("Put task %d to sleep for %d seconds", p->pid, n);
}

static char *sys_kthread_comms[] = {
    "cpuhp/",
    "migration/",
    "ksoftirqd/",
};

int is_sys_kthread(struct task_struct *p) {
  for (int i = 0; i < ARRAY_SIZE(sys_kthread_comms); i++) {
    if (strstarts(p->comm, sys_kthread_comms[i]))
      return 1;
  }
  return 0;
}
