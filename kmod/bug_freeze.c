#include <linux/freezer.h>

#include "kstep.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;

static void controller_init(void) {
  // set_cpus_allowed_ptr(busy_kthread, &mask);
  kstep_sleep();

  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);

  send_sigcode(busy_task, SIGCODE_FORK, 3);
  kstep_sleep();
}

static bool is_ineligible(struct task_struct *p) {
  return strcmp(p->comm, TARGET_TASK) == 0 && p != busy_task && p->on_cpu &&
         ksym.entity_eligible(p->se.cfs_rq, &p->se) == 0;
}

static void controller_body(void) {
  for (int i = 0; i < 20; i++) {
    kstep_tick();
  }
  struct task_struct *pause_task = kstep_tick_until_task(is_ineligible);
  TRACE_INFO("dequeue ineligible task %d", pause_task->pid);
  send_sigcode(pause_task, SIGCODE_SLEEP, 1);

  kstep_tick();

  TRACE_INFO("freeze ineligible task %d", pause_task->pid);

// https://github.com/torvalds/linux/commit/f5d39b020809146cc28e6e73369bf8065e0310aa
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  static_branch_inc(&freezer_active);
#else
  atomic_inc(&system_freezing_cnt);
#endif

  *ksym.pm_freezing = true;
  ksym.freeze_task(pause_task);

  *ksym.pm_freezing = false;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 1, 0)
  static_branch_dec(&freezer_active);
#else
  atomic_dec(&system_freezing_cnt);
#endif
  kstep_sleep();

  kstep_tick();
  kstep_tick();

  send_sigcode(pause_task, SIGCODE_UNKNOWN, 0);
  print_tasks();
  kstep_sleep();

  for (int i = 0; i < 20; i++) {
    kstep_tick();
  }
}

struct controller_ops controller_freeze = {
    .name = "freeze",
    .init = controller_init,
    .body = controller_body,
};
