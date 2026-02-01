// https://github.com/torvalds/linux/commit/bbce3de72be56e4b5f68924b7da9630cc89aa1a8
#include "driver.h"
#include "internal.h" // dequeue_entities

// https://github.com/torvalds/linux/commit/fab4a808ba9fb59b691d7096eed9b1494812ffd6
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
static struct task_struct *special_task;
static struct task_struct *starved_task;
static struct task_struct *other_task;

static void setup(void) {
  kstep_cgroup_create("g0");
  kstep_cgroup_create("g1");
  kstep_cgroup_set_weight("g0", 15);
  kstep_cgroup_set_weight("g1", 15);

  special_task = kstep_task_create();
  starved_task = kstep_task_create();
  other_task = kstep_task_create();

  kstep_cgroup_add_task("g0", special_task->pid);
  kstep_cgroup_add_task("g0", starved_task->pid);
  kstep_cgroup_add_task("g1", other_task->pid);
}

static void *ineligible_group_with_eligible_tasks(void) {
  if (special_task->on_cpu && !kstep_eligible(special_task->se.parent) &&
      kstep_eligible(&special_task->se))
    return special_task;
  return NULL;
}

static void run(void) {
  kstep_task_wakeup(special_task);
  kstep_task_wakeup(other_task);

  kstep_tick_repeat(10);
  kstep_tick_until(ineligible_group_with_eligible_tasks);

  // Pause ineligible task
  kstep_task_pause(special_task);

  // Dequeue ineligible group
  struct sched_entity *group_se = special_task->se.parent;
  typedef void(dequeue_entities_type)(struct cfs_rq * cfs_rq,
                                      struct sched_entity * se, int flags);
  KSYM_IMPORT_TYPED(dequeue_entities_type, dequeue_entities);
  KSYM_dequeue_entities(group_se->cfs_rq, group_se, DEQUEUE_SLEEP);

  // Reweight the group
  kstep_cgroup_set_weight("g0", 16);

  kstep_task_wakeup(starved_task);

  kstep_tick_repeat(18);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "vruntime_overflow",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_tasks = true,
    .print_rq = true,
};
