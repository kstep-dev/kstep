#include <uapi/linux/sched.h>
#include <uapi/linux/sched/types.h>

#include "driver.h"

static struct task_struct *task;

static void set_task_uclamp(struct task_struct *p, unsigned int min,
                            unsigned int max) {
  struct sched_attr attr = {
      .size = sizeof(attr),
      .sched_policy = p->policy,
      .sched_flags = SCHED_FLAG_UTIL_CLAMP,
      .sched_util_min = min,
      .sched_util_max = max,
      .sched_nice = task_nice(p),
  };

  int ret = sched_setattr_nocheck(p, &attr);
  if (ret)
    panic("sched_setattr_nocheck failed: %d", ret);
}

static void setup(void) {
  task = kstep_task_create();

  kstep_cgroup_create_pinned("uclamp", "1");
  kstep_cgroup_write("uclamp", "cpu.uclamp.min", "0.00");
  kstep_cgroup_write("uclamp", "cpu.uclamp.max", "50.00");
  kstep_cgroup_add_task("uclamp", task->pid);

  set_task_uclamp(task, SCHED_CAPACITY_SCALE * 60 / 100,
                  SCHED_CAPACITY_SCALE * 80 / 100);
}

static bool is_uclamp_active(struct task_struct *p) {
  return p->uclamp[UCLAMP_MIN].active && p->uclamp[UCLAMP_MAX].active;
}

static void run(void) {
  kstep_task_wakeup(task);

  if (!is_uclamp_active(task))
    panic("uclamp did not become active for pid %d", task->pid);

  unsigned int min = task->uclamp[UCLAMP_MIN].value;
  unsigned int max = task->uclamp[UCLAMP_MAX].value;

  if (min > max)
    TRACE_INFO("uclamp inversion detected: min=%u max=%u", min, max);
  else
    TRACE_INFO("uclamp inversion not detected: min=%u max=%u", min, max);
}

struct kstep_driver uclamp_inversion = {
    .name = "uclamp_inversion",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
    .print_tasks = true,
};
