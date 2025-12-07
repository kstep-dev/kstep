#include "kstep.h"

static struct task_struct *spcial_task;
static struct task_struct *starved_task;
static struct task_struct *other_task;

static void init(void) {
  kstep_cgroup_create_pinned("g0", "1");
  kstep_cgroup_create_pinned("g1", "1");
  kstep_cgroup_set_weight("g0", 15);
  kstep_cgroup_set_weight("g1", 15);

  spcial_task = kstep_task_create();
  starved_task = kstep_task_create();
  other_task = kstep_task_create();

  kstep_cgroup_add_task("g0", spcial_task->pid);
  kstep_cgroup_add_task("g0", starved_task->pid);
  kstep_cgroup_add_task("g1", other_task->pid);
}

static void *ineligible_group_with_eligible_tasks(void) {
  if (spcial_task->on_cpu && !kstep_eligible(spcial_task->se.parent) &&
      kstep_eligible(&spcial_task->se))
    return spcial_task;
  return NULL;
}

static void body(void) {
  kstep_task_wakeup(spcial_task);
  kstep_task_wakeup(other_task);

  kstep_tick_repeat(10);
  kstep_tick_until(ineligible_group_with_eligible_tasks);

  // Pause ineligible task
  kstep_task_pause(spcial_task);

  // Dequeue ineligible group
  struct sched_entity *group_se = spcial_task->se.parent;
  ksym.dequeue_entities(group_se->cfs_rq, group_se, DEQUEUE_SLEEP);

  // Reweight the group
  kstep_cgroup_set_weight("g0", 16);

  kstep_task_wakeup(starved_task);

  kstep_tick_repeat(18);
}

struct kstep_driver vruntime_overflow = {
    .name = "vruntime_overflow",
    .init = init,
    .body = body,
};
