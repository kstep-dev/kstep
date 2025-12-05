#include "kstep.h"

#define MAX_TASKS 128

static int task_to_cgroup_id[MAX_TASKS][2];

static void record_task_to_groups(int expected_count, int level_id,
                                  int id_in_level) {
  struct task_struct *p;
  int count = 0;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, busy_task->comm) == 0 &&
          task_to_cgroup_id[p->pid - busy_task->pid][0] == -1) {
        task_to_cgroup_id[p->pid - busy_task->pid][0] = level_id;
        task_to_cgroup_id[p->pid - busy_task->pid][1] = id_in_level;
        if (++count == expected_count) {
          return;
        }
      }
    }
    kstep_sleep();
    TRACE_INFO("Waiting for recording task_to_cgroup_id: %d level_id: %d "
               "id_in_level: %d",
               expected_count, level_id, id_in_level);
  }
}

static struct task_struct *ineligible_task = NULL;
static struct sched_entity *ineligible_tg_se = NULL;
static int cpu_of_ineligible_task = -1;

static bool is_ineligible(struct task_struct *p) {
  struct sched_entity *se = &p->se;
  return strcmp(p->comm, busy_task->comm) == 0 && p != busy_task && p->on_cpu &&
         ksym.entity_eligible(se->parent->cfs_rq, se->parent) == 0 &&
         ksym.entity_eligible(se->cfs_rq, se) == 1 &&
         task_to_cgroup_id[p->pid - busy_task->pid][0] == 3;
}

static void sleep_all_tasks_in_ineligible_tg(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, busy_task->comm) != 0 || p == busy_task)
      continue;
    if (p->se.parent != ineligible_tg_se)
      continue;
    kstep_task_pause(p);
  }
}

static struct task_struct *get_curr_task(int cpu) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == cpu && p->on_cpu == 1)
      return p;
  }
  return NULL;
}

static void controller_body(void) {
  for (int i = 0; i < MAX_TASKS; i++) {
    task_to_cgroup_id[i][0] = -1;
    task_to_cgroup_id[i][1] = -1;
  }
  // the first busy task is in l0_0 by default
  task_to_cgroup_id[busy_task->pid - busy_task->pid][0] = 0;
  task_to_cgroup_id[busy_task->pid - busy_task->pid][1] = 0;

  /*
    create a cgroup tree
    root -> l1_0 -> l2_0 -> l3_0
                 |      |-> l3_1
                 |
                 |--> l2_1
  */
  kstep_cgroup_create("l1_0", "1");
  kstep_cgroup_create("l1_0/l2_0", "1");
  kstep_cgroup_create("l1_0/l2_0/l3_0", "1");
  kstep_cgroup_create("l1_0/l2_0/l3_1", "1");
  kstep_cgroup_create("l1_0/l2_1", "1");

  // set up the configuration for the cgroup tree
  kstep_cgroup_write_file("l1_0/l2_0/l3_0", "cpu.weight", "20");

  // create 1 task in l3_0
  kstep_task_signal(busy_task, SIGCODE_CLONE3_L3_0, 1, 0, 0);
  record_task_to_groups(1, 3, 0);
  // create 3 tasks in l3_1
  kstep_task_signal(busy_task, SIGCODE_CLONE3_L3_1, 3, 0, 0);
  record_task_to_groups(3, 3, 1);

  kstep_task_pause(busy_task);

  kstep_tick_repeat(10);

  // tick until there is a not eligible task group with eligible tasks
  ineligible_task = kstep_tick_until_task(is_ineligible);
  ineligible_tg_se = ineligible_task->se.parent;
  cpu_of_ineligible_task = task_cpu(ineligible_task);
  TRACE_INFO("Found not eligible task group");
  // pause all tasks in the not eligible task group
  sleep_all_tasks_in_ineligible_tg();

  ksym.dequeue_entities(ineligible_tg_se->cfs_rq, ineligible_tg_se,
                        DEQUEUE_SLEEP);
  struct task_struct *p = get_curr_task(cpu_of_ineligible_task);
  kstep_cgroup_write_file("l1_0/l2_0/l3_0", "cpu.weight", "100");

  kstep_task_signal(p, SIGCODE_CLONE3_L3_0, 1, 0, 0);

  kstep_tick_repeat(18);
}

struct controller_ops controller_vruntime_overflow = {
    .name = "vruntime_overflow",
    .body = controller_body,
};
