#include "kstep.h"

#define TARGET_TASK "test-proc"
#define CGROUP_TASK "cgroup-proc"
# define MAX_TASKS 128

static struct task_struct *busy_task = NULL;
static struct task_struct *cgroup_task = NULL;

static int task_to_cgroup_id[MAX_TASKS][2];

static void record_task_to_groups(int expected_count, int level_id, int id_in_level) {
  struct task_struct *p;
  int count = 0;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0 &&
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

static void controller_init(void) {
  for (int i = 0; i < MAX_TASKS; i++) {
    task_to_cgroup_id[i][0] = -1;
    task_to_cgroup_id[i][1] = -1;
  }
  // the first busy task is in l0_0 by default
  task_to_cgroup_id[busy_task->pid - busy_task->pid][0] = 0;
  task_to_cgroup_id[busy_task->pid - busy_task->pid][1] = 0;

  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);

  cgroup_task = poll_task(CGROUP_TASK);
  /*
    create a cgroup tree
    root -> l1_0 -> l2_0 -> l3_0
                 |      |-> l3_1
                 |
                 |--> l2_1
  */
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 1 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 1 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 2 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 2 << 16 | 0x0);

  // set up the configuration for the cgroup tree
  send_sigcode2(cgroup_task, SIGCODE_REWEIGHT_CGROUP, 3 << 16 | 0x0, 20);
  send_sigcode2(cgroup_task, SIGCODE_SETCPU_CGROUP, 1 << 16 | 0x0, 1);
  send_sigcode2(cgroup_task, SIGCODE_SETCPU_CGROUP, 2 << 16 | 0x0, 1);
  send_sigcode2(cgroup_task, SIGCODE_SETCPU_CGROUP, 2 << 16 | 0x1, 1);
  send_sigcode2(cgroup_task, SIGCODE_SETCPU_CGROUP, 3 << 16 | 0x0, 1);
  send_sigcode2(cgroup_task, SIGCODE_SETCPU_CGROUP, 3 << 16 | 0x1, 1);

  // create 1 task in l3_0
  send_sigcode(busy_task, SIGCODE_CLONE3_L3_0, 1);
  record_task_to_groups(1, 3, 0);
  // create 3 tasks in l3_1
  send_sigcode(busy_task, SIGCODE_CLONE3_L3_1, 3);
  record_task_to_groups(3, 3, 1);
}

static struct task_struct *ineligible_task = NULL;
static struct sched_entity *ineligible_tg_se = NULL;
static int cpu_of_ineligible_task = -1;

static void find_not_eligible_tg(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task || p->on_cpu == 0)
      continue;
    struct sched_entity *se = &p->se;

    if (ineligible_task == NULL &&
        ksym.entity_eligible(se->parent->cfs_rq, se->parent) == 0 &&
        ksym.entity_eligible(se->cfs_rq, se) == 1 &&
        task_to_cgroup_id[p->pid - busy_task->pid][0] == 3) {
      ineligible_task = p;
      ineligible_tg_se = se->parent;
      cpu_of_ineligible_task = task_cpu(p);
    }
  }
}

static void sleep_all_tasks_in_ineligible_tg(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task)
      continue;
    if (p->se.parent != ineligible_tg_se)
      continue;
    send_sigcode(p, SIGCODE_PAUSE, 0);
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
  // Update clock
  for (int i = 0; i < 20; i++) {
    call_tick_once();
  }

  // tick until there is a not eligible task group with eligible tasks
  while (1) {
    find_not_eligible_tg();
    if (ineligible_tg_se != NULL) {
      TRACE_INFO("Found not eligible task group");
      // pause all tasks in the not eligible task group
      sleep_all_tasks_in_ineligible_tg();
      break;
    }
    call_tick_once();
  }

  ksym.dequeue_entities(ineligible_tg_se->cfs_rq, ineligible_tg_se, DEQUEUE_SLEEP);
  struct task_struct *p = get_curr_task(cpu_of_ineligible_task);
  send_sigcode2(cgroup_task, SIGCODE_REWEIGHT_CGROUP, 3 << 16 | 0x0, 100);

  send_sigcode(p, SIGCODE_CLONE3_L3_0, 1);

  for (int i = 0; i < 60; i++) {
    call_tick_once();
  }
}

struct controller_ops controller_vruntime_overflow = {
    .name = "vruntime_overflow",
    .init = controller_init,
    .body = controller_body,
};
