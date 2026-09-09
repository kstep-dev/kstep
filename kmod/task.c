#include <linux/umh.h>
#include <uapi/linux/sched/types.h>
#include <linux/spinlock.h>

#include "internal.h"
#include <linux/namei.h> // kern_path_create, vfs_mknod
#include <linux/version.h>
#include "user.h"

static struct file *console_file = NULL;
static struct file *null_file = NULL;

static void pipe_create(void);

void kstep_task_init(void) {
  console_file = filp_open("/dev/console", O_WRONLY, 0); // write only
  if (IS_ERR(console_file))
    panic("Failed to open /dev/console");
  null_file = filp_open("/dev/null", O_RDONLY, 0); // read only
  if (IS_ERR(null_file))
    panic("Failed to open /dev/null");
  pipe_create();
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

  // Wait until the helper has completed startup and acknowledged readiness.
  for (int i = 0; i < 100; i++) {
    kstep_sleep();
    if (strcmp(p->comm, TASK_READY_COMM) == 0) {
      TRACE_INFO("Task %d is ready", p->pid);
      kstep_task_pin(p, 1, num_online_cpus() - 1);
      kstep_reset_task(p);
      return p;
    }
    TRACE_INFO("Waiting for task %d to become ready", p->pid);
  }
  panic("Task %d did not start", p->pid);
}

static void kstep_task_signal(struct task_struct *p, enum sigcode code,
                              int val) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1, .si_code = code, .si_int = val};
  kstep_cov_enable_controller();
  send_sig_info(SIGUSR1, &info, p);
  kstep_cov_disable_controller();
  kstep_sleep();
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

// wait/post: a counting semaphore shared by all tasks, implemented as one pipe whose bytes
// are the tokens. wait sleeps in pipe_read() (TASK_INTERRUPTIBLE on the pipe's wait queue)
// until a token is there and consumes it; post writes a token, and pipe_write() wakes one
// waiter through wake_up_interruptible_sync_poll(), i.e. a WF_SYNC wakeup issued from the
// poster's CPU: the production sync-wakeup path. A post with no waiter leaves a token, so the
// next wait returns at once (a poster that should sleep uses kstep_task_pause).
static void pipe_create(void) {
  struct path parent;
  struct dentry *dentry;
  int err;

  // kern_path_create/done_path_create became start_creating_path/end_creating_path and
  // vfs_mknod gained a delegation argument in 6.19 (directory delegations).
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  dentry = start_creating_path(AT_FDCWD, PIPE_PATH, &parent, 0);
  if (IS_ERR(dentry))
    panic("kstep: cannot create %s: %ld", PIPE_PATH, PTR_ERR(dentry));
  err = vfs_mknod(mnt_idmap(parent.mnt), d_inode(parent.dentry), dentry, S_IFIFO | 0600, 0, NULL);
  end_creating_path(&parent, dentry);
#else
  dentry = kern_path_create(AT_FDCWD, PIPE_PATH, &parent, 0);
  if (IS_ERR(dentry))
    panic("kstep: cannot create %s: %ld", PIPE_PATH, PTR_ERR(dentry));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
  err = vfs_mknod(mnt_idmap(parent.mnt), d_inode(parent.dentry), dentry, S_IFIFO | 0600, 0);
#else
  err = vfs_mknod(mnt_user_ns(parent.mnt), d_inode(parent.dentry), dentry, S_IFIFO | 0600, 0);
#endif
  done_path_create(&parent, dentry);
#endif
  if (err)
    panic("kstep: mknod %s failed: %d", PIPE_PATH, err);
}

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

void kstep_task_set_prio(struct task_struct *p, int prio) {
  kstep_cov_enable_controller();
  set_user_nice(p, prio);
  kstep_cov_disable_controller();
  TRACE_INFO("Set priority of task %d to %d", p->pid, prio);
  kstep_sleep();
}

void kstep_task_fifo(struct task_struct *p) {
  struct sched_attr attr = {
      .sched_policy = SCHED_FIFO,
      .sched_priority = 80,
  };
  kstep_cov_enable_controller();
  sched_setattr_nocheck(p, &attr);
  kstep_cov_disable_controller();
  TRACE_INFO("Set task %d to FIFO", p->pid);
  kstep_sleep();
}

void kstep_task_cfs(struct task_struct *p) {
  struct sched_attr attr = {
      .sched_policy = SCHED_NORMAL,
      .sched_nice = 0,
  };
  kstep_cov_enable_controller();
  sched_setattr_nocheck(p, &attr);
  kstep_cov_disable_controller();
  TRACE_INFO("Set task %d to CFS", p->pid);
  kstep_sleep();
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
  kstep_sleep();
}

void kstep_task_kernel_pause(struct task_struct *p) {
  // Set TASK_INTERRUPTIBLE so that both signal_wakeup (sends SIGUSR1, which
  // wakes TASK_INTERRUPTIBLE) and kernel_wakeup (wake_up_process, which wakes
  // any sleeping state) can wake the task. try_to_block_task() will check
  // signal_pending() and abort the block if signals are pending, but since
  // kSTEP controls signal delivery there are no stray signals at pause time.
  WRITE_ONCE(p->__state, TASK_INTERRUPTIBLE);

  // Trigger a reschedule on the task's CPU. When returning to userspace,
  // exit_to_user_mode_loop() calls schedule(), which will see the
  // non-RUNNING state and dequeue the task via the normal blocking path.
  kstep_cov_enable_controller();
  set_tsk_thread_flag(p, TIF_NEED_RESCHED);
  kick_process(p);
  kstep_cov_disable_controller();

  kstep_sleep();
  TRACE_INFO("Paused task %d (kernel)", p->pid);
}

void kstep_task_kernel_wakeup(struct task_struct *p) {
  kstep_cov_enable_controller();
  wake_up_process(p);
  kstep_cov_disable_controller();
  TRACE_INFO("Waked up task %d (kernel)", p->pid);
}
