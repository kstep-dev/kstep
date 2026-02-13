#include <linux/umh.h>

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
  
  char cov_mode_arg[2] = {0};
  cov_mode_arg[0] = kstep_cov_mode_enabled() ? '1' : '0';
  char *argv[] = {path, cov_mode_arg, NULL};

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
    if (strcmp(p->comm, TASK_READY_COMM) == 0)
      return p;
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
  send_sig_info(SIGUSR1, &info, p);
  kstep_sleep();
}

void kstep_task_fork(struct task_struct *p, int n) {
  kstep_task_signal(p, SIGCODE_FORK, n, 0, 0);
  TRACE_INFO("Forked task %d %d times", p->pid, n);
}

void kstep_task_fifo(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_FIFO, 0, 0, 0);
  TRACE_INFO("Set task %d to FIFO", p->pid);
}

void kstep_task_pin(struct task_struct *p, int begin, int end) {
  kstep_task_signal(p, SIGCODE_PIN, begin, end, 0);
  // Avoid high-volume logs in migration-heavy reproducers.
  // TRACE_INFO("Pinned task %d to CPUs %d-%d", p->pid, begin, end);
}

void kstep_task_pause(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_PAUSE, 0, 0, 0);
  TRACE_INFO("Paused task %d", p->pid);
}

void kstep_task_wakeup(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_WAKEUP, 0, 0, 0);
  TRACE_INFO("Waked up task %d", p->pid);
}

void kstep_task_usleep(struct task_struct *p, int us) {
  kstep_task_signal(p, SIGCODE_USLEEP, us, 0, 0);
  TRACE_INFO("Put task %d to sleep for %d us", p->pid, us);
}

void kstep_task_set_prio(struct task_struct *p, int prio) {
  kstep_task_signal(p, SIGCODE_SET_PRIO, prio, 0, 0);
  TRACE_INFO("Set priority of task %d to %d", p->pid, prio);
}

void kstep_task_kcov_dump(struct task_struct *p) {
  kstep_task_signal(p, SIGCODE_KCOV_DUMP, 0, 0, 0);
  TRACE_INFO("Dumped kcov of task %d", p->pid);
}
