// https://github.com/torvalds/linux/commit/1862d8e264de

#include <linux/version.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 19, 0)

static struct task_struct *task;

static void setup(void) { task = kstep_task_create(); }

static void run(void) {
  kstep_task_pin(task, 1, 1);
  kstep_task_wakeup(task);
  kstep_tick_repeat(3);

  // Boost to RT FIFO directly from kernel context
  struct sched_attr attr_rt = {
      .size = sizeof(struct sched_attr),
      .sched_policy = SCHED_FIFO,
      .sched_priority = 50,
  };
  int ret = sched_setattr_nocheck(task, &attr_rt);
  if (ret)
    panic("sched_setattr_nocheck (RT) failed: %d", ret);
  TRACE_INFO("Boosted task %d to RT FIFO", task->pid);

  kstep_tick_repeat(3);

  // Put task to sleep so it's not running
  kstep_task_usleep(task, 1000000);
  kstep_tick_repeat(2);

  TRACE_INFO("Task %d state=%u on_rq=%d on_cpu=%d class=%ps", task->pid,
             task->__state, task->on_rq, task->on_cpu,
             task->sched_class);

  // Demote sleeping RT task to CFS - triggers WARN_ON in sched_change_end()
  struct sched_attr attr_cfs = {
      .size = sizeof(struct sched_attr),
      .sched_policy = SCHED_NORMAL,
      .sched_priority = 0,
  };
  ret = sched_setattr_nocheck(task, &attr_cfs);
  if (ret)
    panic("sched_setattr_nocheck (CFS) failed: %d", ret);
  TRACE_INFO("Demoted task %d to CFS", task->pid);

  kstep_tick_repeat(5);
}

static void on_tick_begin(void) { kstep_output_curr_task(); }

KSTEP_DRIVER_DEFINE{
    .name = "sched_change_assert",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 10000,
};

#else
static void run(void) { TRACE_INFO("Skipped: wrong kernel version"); }
KSTEP_DRIVER_DEFINE{.name = "sched_change_assert", .run = run};
#endif
