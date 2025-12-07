#include "kstep.h"

static struct task_struct *tasks[4];

static void init(void) {
  // Setup cgroups
  kstep_cgroup_create_pinned("g0", "1");
  kstep_cgroup_create_pinned("g1", "1");
  kstep_cgroup_set_weight("g0", 20);

  // 2 tasks in each group
  for (int i = 0; i < ARRAY_SIZE(tasks); i++) {
    tasks[i] = kstep_task_create();
    kstep_cgroup_add_task(i < 2 ? "g0" : "g1", tasks[i]->pid);
  }
}

static void *ineligible_group_with_eligible_tasks(void) {
  struct task_struct *p = tasks[0];
  if (p->on_cpu && !kstep_eligible(p->se.parent) && kstep_eligible(&p->se))
    return p;
  return NULL;
}

static void body(void) {
  // Wake up all tasks except for tasks[1]
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    if (i != 1)
      kstep_task_wakeup(tasks[i]);

  kstep_tick_repeat(10);

  kstep_tick_until(ineligible_group_with_eligible_tasks);

  // Pause ineligible task
  kstep_task_pause(tasks[0]);

  // Dequeue ineligible group
  struct sched_entity *group_se = tasks[0]->se.parent;
  ksym.dequeue_entities(group_se->cfs_rq, group_se, DEQUEUE_SLEEP);

  // Reweight the group
  kstep_cgroup_set_weight("g0", 100);

  // Starvation for tasks[1]
  kstep_task_wakeup(tasks[1]);

  kstep_tick_repeat(18);
}

struct kstep_driver vruntime_overflow = {
    .name = "vruntime_overflow",
    .init = init,
    .body = body,
};
