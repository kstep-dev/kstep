#include <linux/version.h>

#include "driver.h"
#include "internal.h" // rq

// https://github.com/torvalds/linux/commit/abc158c82ae555078aa5dd2d8407c3df0f868904
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)

static struct task_struct *target_task;
static struct task_struct *other_task;

static void setup(void) {
  kstep_cgroup_create_pinned("g0", "1");
  kstep_cgroup_create_pinned("g1", "1");

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

  struct rq *rq = cpu_rq(1);
  TRACE_INFO("rq->cfs.h_nr_running=%u, rq->cfs.h_nr_delayed=%u",
             rq->cfs.h_nr_running, rq->cfs.h_nr_delayed);
  if (rq->cfs.h_nr_delayed == 1)
    TRACE_INFO("h_nr_runnable accounted incorrectly");
  else
    TRACE_INFO("h_nr_runnable accounted correctly");
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

struct kstep_driver h_nr_runnable = {
    .name = "h_nr_runnable",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_rq = true,
    .print_tasks = true,
};
