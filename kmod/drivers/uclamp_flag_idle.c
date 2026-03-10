// https://github.com/torvalds/linux/commit/ca4984a7dd863f3e1c0df775ae3e744bff24c303
// Bug: UCLAMP_FLAG_IDLE gets stuck when uclamp_update_active() does dec+inc
// and transiently reaches 0 active uclamp tasks during the dec.

#include <linux/version.h>
#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 13, 0)

static struct task_struct *task;

static void setup(void) {
  kstep_cgroup_create("ucidle");
  kstep_cgroup_write("ucidle", "cpu.uclamp.max", "50.00");

  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
  kstep_cgroup_add_task("ucidle", task->pid);
}

static void run(void) {
  struct rq *rq = cpu_rq(1);

  kstep_task_wakeup(task);
  kstep_tick();

  unsigned int flags_before = rq->uclamp_flags;
  unsigned int nr_before = rq->nr_running;
  TRACE_INFO("before: uclamp_flags=0x%x nr_running=%u uclamp_max_active=%d",
             flags_before, nr_before, task->uclamp[UCLAMP_MAX].active);

  // Changing cgroup uclamp.max triggers cpu_util_update_eff() ->
  // uclamp_update_active_tasks() -> uclamp_update_active() on our task.
  // The dec-then-inc in uclamp_update_active() transiently hits 0 active
  // tasks, setting UCLAMP_FLAG_IDLE, but the inc fails to clear it.
  kstep_cgroup_write("ucidle", "cpu.uclamp.max", "80.00");

  unsigned int flags_after = rq->uclamp_flags;
  unsigned int nr_after = rq->nr_running;
  TRACE_INFO("after: uclamp_flags=0x%x nr_running=%u", flags_after, nr_after);

  bool idle_flag_set = flags_after & UCLAMP_FLAG_IDLE;
  bool tasks_present = nr_after > 0;

  if (idle_flag_set && tasks_present) {
    kstep_fail("UCLAMP_FLAG_IDLE stuck: flags=0x%x nr_running=%u",
               flags_after, nr_after);
  } else {
    kstep_pass("UCLAMP_FLAG_IDLE correct: flags=0x%x nr_running=%u",
               flags_after, nr_after);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "uclamp_flag_idle",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "uclamp_flag_idle",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
