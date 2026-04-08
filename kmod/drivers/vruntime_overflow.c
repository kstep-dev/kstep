// https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8
#include "driver.h"

static struct task_struct *special_task;
static struct task_struct *starved_task;
static struct task_struct *other_task;

static void setup(void) {
  kstep_cgroup_create("g0");
  kstep_cgroup_create("g0/g1");
  kstep_cgroup_create("g0/g2");
  kstep_cgroup_set_weight("g0/g2", 15);
  kstep_cgroup_set_weight("g0/g1", 15);

  special_task = kstep_task_create();
  starved_task = kstep_task_create();
  other_task = kstep_task_create();

  kstep_cgroup_add_task("g0/g2", special_task->pid);
  kstep_cgroup_add_task("g0/g1", other_task->pid);
}

static void *ineligible_group_with_eligible_tasks(void) {
  if (special_task->on_cpu && 
      !kstep_eligible(special_task->se.parent) && 
      !kstep_eligible(special_task->se.parent->parent) &&
      kstep_eligible(&special_task->se))
    return special_task;
  return NULL;
}

static void run(void) {
  kstep_task_wakeup(special_task);
  kstep_task_wakeup(other_task);
  kstep_task_wakeup(starved_task);

  kstep_tick_repeat(10);
  kstep_tick_until(ineligible_group_with_eligible_tasks);

  kstep_cgroup_add_task("", special_task->pid);

  kstep_cgroup_add_task("", other_task->pid);

  kstep_cgroup_set_weight("g0", 105);

  kstep_cgroup_destroy("g0/g2");

  kstep_cgroup_set_weight("g0", 106);

  kstep_cgroup_add_task("g0/g1", starved_task->pid);

  kstep_tick_repeat(20);
}

KSTEP_DRIVER_DEFINE{
    .name = "vruntime_overflow",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 1000,
};
