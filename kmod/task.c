#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/umh.h>

#include "kstep.h"

static void run_prog(char *path, int (*init)(struct subprocess_info *info,
                                             struct cred *new)) {
  struct subprocess_info *info = call_usermodehelper_setup(
      path, (char *[]){path, NULL}, NULL, GFP_KERNEL, init, NULL, NULL);
  if (info == NULL)
    panic("Failed to setup user mode helper");

  if (call_usermodehelper_exec(info, UMH_WAIT_EXEC) < 0)
    panic("Failed to run user mode helper");
}

struct file *console_file = NULL;
struct task_struct *cgroup_task = NULL;
struct task_struct *busy_task = NULL;

// Redirect stdout and stderr to /dev/console
// Reference: `console_on_rootfs` and `init_dup` in `init/main.c`
static void init_task_console(void) {
  for (int i = 0; i < 3; i++) { // stdin, stdout, stderr
    int fd = get_unused_fd_flags(0);
    if (fd < 0)
      panic("Failed to get unused fd");
    fd_install(fd, get_file(console_file));
  }
}

static int cgroup_task_init(struct subprocess_info *info, struct cred *new) {
  init_task_console();
  cgroup_task = current;
  TRACE_INFO("cgroup task created with pid %d", cgroup_task->pid);
  kstep_sleep();
  return 0;
}

static int busy_task_init(struct subprocess_info *info, struct cred *new) {
  init_task_console();
  busy_task = current;
  TRACE_INFO("busy task created with pid %d", busy_task->pid);
  kstep_sleep();
  return 0;
}

void kstep_tasks_init(void) {
  console_file = filp_open("/dev/console", O_RDWR, 0);
  if (IS_ERR(console_file))
    panic("Failed to open /dev/console");
  run_prog("/cgroup", cgroup_task_init);
  run_prog("/busy", busy_task_init);
  fput(console_file);
}

void send_sigcode3(struct task_struct *p, enum sigcode code, int val1, int val2,
                   int val3) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      ._sifields = {._rt = {._sigval = {val1}, ._pid = val2, ._uid = val3}}};
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (val1=%d, val2=%d, val3=%d) to pid %d",
             sigcode_to_str[code], val1, val2, val3, p->pid);
  kstep_sleep();
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
