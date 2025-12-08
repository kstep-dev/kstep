#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/umh.h>

#include "kstep.h"
#include "user.h"

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

struct kstep_task_info {
  struct task_struct *task;
};

static int task_init(struct subprocess_info *info, struct cred *new) {
  struct kstep_task_info *task_info = info->data;
  init_task_console();
  task_info->task = current;
  TRACE_INFO("Task created with pid %d", current->pid);
  return 0;
}

struct task_struct *kstep_task_create(void) {
  char *path = "/task";
  char *argv[] = {path, NULL};
  struct kstep_task_info task_info = {};
  struct subprocess_info *info = call_usermodehelper_setup(
      path, argv, NULL, GFP_KERNEL, task_init, NULL, &task_info);
  if (info == NULL)
    panic("Failed to setup user mode helper");

  if (call_usermodehelper_exec(info, UMH_WAIT_EXEC) < 0)
    panic("Failed to run user mode helper");

  // Wait for the task to start
  kstep_sleep();
  struct task_struct *p = task_info.task;
  for (int i = 0; i < 100; i++) {
    if (strcmp(p->comm, TASK_READY_COMM) == 0)
      return p;
    kstep_sleep();
    TRACE_INFO("Waiting for task %d to start", p->pid);
  }
  panic("Task %d did not start", p->pid);
}

void kstep_task_signal(struct task_struct *p, enum sigcode code, int val1,
                       int val2, int val3) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      ._sifields = {._rt = {._sigval = {val1}, ._pid = val2, ._uid = val3}}};
  send_sig_info(SIGUSR1, &info, p);
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


void kstep_task_fifo(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_FIFO, 0, 0, 0);
  TRACE_INFO("Set task %d to FIFO", p->pid);
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

void kstep_task_set_prio(struct task_struct *p, int prio) {
  kstep_task_signal(p, SIGCODE_SET_PRIO, prio, 0, 0);
  TRACE_INFO("Set priority of task %d to %d", p->pid, prio);
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
