#include "kstep.h"

static struct task_struct *target_task;
static struct task_struct *other_task;

static void setup(void) {
  // Create target task and add it to group g0
  target_task = kstep_task_create();
  kstep_cgroup_create_pinned("g0", "1");
  kstep_cgroup_add_task("g0", target_task->pid);

  // Create other tasks
  other_task = kstep_task_create();
}

static void run(void) {
  kstep_task_set_prio(other_task, 19);

  kstep_task_wakeup(target_task);

  kstep_tick_repeat(5);

  kstep_cgroup_set_weight("g0", 10000);

  kstep_tick_repeat(5);
}

struct kstep_driver lag_vruntime = {
    .name = "lag_vruntime",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_rq = true,
};
