// https://github.com/torvalds/linux/commit/3429dd57f0deb1a602c2624a1dd7c4c11b6c4734

#include <linux/version.h>

#include "driver.h"
#include "internal.h" // rq

// https://github.com/torvalds/linux/commit/76f2f783294d7d55c2564e2dfb0a7279ba0bc264
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)

static struct task_struct *target_task;
static struct task_struct *other_task;

static void setup(void) {
  kstep_cgroup_create("g0");
  kstep_cgroup_create("g1");

  target_task = kstep_task_create();
  other_task = kstep_task_create();

  kstep_cgroup_add_task("g0", target_task->pid);
  kstep_cgroup_add_task("g1", other_task->pid);
}

static void *group_ineligible(void) {
  struct sched_entity *group_se = target_task->se.parent;
  if (target_task->on_cpu && group_se && !group_se->sched_delayed &&
      !kstep_eligible(group_se) && kstep_eligible(&target_task->se))
    return target_task;
  return NULL;
}

static void run(void) {
  kstep_task_wakeup(target_task);
  kstep_task_wakeup(other_task);

  kstep_tick_repeat(5);

  kstep_tick_until(group_ineligible);
  kstep_task_pause(target_task);

  struct cfs_rq *cfs_rq = &cpu_rq(1)->cfs;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
  int h_nr_runnable = cfs_rq->h_nr_runnable;
#else
  int h_nr_runnable = cfs_rq->h_nr_running - cfs_rq->h_nr_delayed;
#endif

  TRACE_INFO("h_nr_runnable == %u", h_nr_runnable);
  if (h_nr_runnable == 1)
    TRACE_INFO("h_nr_runnable accounted correctly");
  else
    TRACE_INFO("h_nr_runnable accounted incorrectly");

  kstep_tick_repeat(11);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "h_nr_runnable",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
};
