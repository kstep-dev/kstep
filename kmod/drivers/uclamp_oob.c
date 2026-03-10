// https://github.com/torvalds/linux/commit/6d2f8909a5fabb73fe2a63918117943986c39b6c
// sched: Fix out-of-bound access in uclamp
//
// With UCLAMP_BUCKETS=20, UCLAMP_BUCKET_DELTA = DIV_ROUND_CLOSEST(1024,20)=51
// uclamp_bucket_id(1024) = 1024/51 = 20, but valid range is [0,19].

#include <linux/version.h>
#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 12, 0)

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = task->policy,
      .sched_flags = SCHED_FLAG_UTIL_CLAMP,
      .sched_util_min = 0,
      .sched_util_max = SCHED_CAPACITY_SCALE,
      .sched_nice = task_nice(task),
  };

  int ret = sched_setattr_nocheck(task, &attr);
  if (ret)
    panic("sched_setattr_nocheck failed: %d", ret);

  unsigned int bucket_id = task->uclamp_req[UCLAMP_MAX].bucket_id;
  unsigned int value = task->uclamp_req[UCLAMP_MAX].value;

  TRACE_INFO("uclamp_req max: value=%u bucket_id=%u UCLAMP_BUCKETS=%d",
             value, bucket_id, UCLAMP_BUCKETS);

  if (bucket_id >= UCLAMP_BUCKETS) {
    kstep_fail("out-of-bound bucket_id=%u >= UCLAMP_BUCKETS=%d (value=%u)",
               bucket_id, UCLAMP_BUCKETS, value);
  } else {
    kstep_pass("bucket_id=%u < UCLAMP_BUCKETS=%d (value=%u)", bucket_id,
               UCLAMP_BUCKETS, value);
  }

  // Trigger enqueue with the OOB bucket_id
  kstep_task_wakeup(task);
  kstep_tick_repeat(5);
  kstep_task_pause(task);
}

KSTEP_DRIVER_DEFINE{
    .name = "uclamp_oob",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "uclamp_oob",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
