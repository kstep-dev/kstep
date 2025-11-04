#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "utils.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task = NULL;
static struct task_struct *cgroup_task = NULL;

static void poll_target_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    TRACE_DEBUG("pid=%d, comm=%s, state=%x, on_cpu=%d", p->pid, p->comm,
                p->__state, p->on_cpu);
  }
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0)
        busy_task = p;
      if (strcmp(p->comm, "cgroup-proc") == 0)
        cgroup_task = p;
      if (cgroup_task != NULL && busy_task != NULL)
        return;
    }
    udelay(SIM_INTERVAL_US);
    TRACE_INFO("Waiting for process %s to be created", TARGET_TASK);
  }
}

static int task_to_cgroup_id[128][2];

static void record_task_groups(int val, int level_id, int id_in_level) {
  struct task_struct *p;
  int count = 0;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0 &&
          task_to_cgroup_id[p->pid - busy_task->pid][0] == -1) {
        TRACE_INFO(
            "Recording task_to_cgroup_id: %d level_id: %d id_in_level: %d",
            p->pid, level_id, id_in_level);
        task_to_cgroup_id[p->pid - busy_task->pid][0] = level_id;
        task_to_cgroup_id[p->pid - busy_task->pid][1] = id_in_level;
        if (++count == val) {
          return;
        }
      }
    }
    udelay(SIM_INTERVAL_US);
    TRACE_INFO("Waiting for recording task_to_cgroup_id: %d level_id: %d "
               "id_in_level: %d",
               val, level_id, id_in_level);
  }
}

static void controller_init(void) {
  int cpu;
  for (int i = 0; i < 128; i++) {
    task_to_cgroup_id[i][0] = -1;
    task_to_cgroup_id[i][1] = -1;
  }
  task_to_cgroup_id[busy_task->pid - busy_task->pid][0] = 0;
  task_to_cgroup_id[busy_task->pid - busy_task->pid][1] = 0;

  poll_target_task();
  TRACE_INFO("Found busy task: %d, cgroup task: %d", busy_task->pid,
             cgroup_task->pid);

  reset_task_stats(busy_task);

  /*
    create a cgroup tree
    root -> l1_0 -> l2_0 -> l3_0
                 -> l2_1 -> l3_1
  */

  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 1 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 1 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 2 << 16 | 0x0);
  send_sigcode(cgroup_task, SIGCODE_CGROUP_CREATE, 2 << 16 | 0x0);

  send_sigcode(busy_task, SIGCODE_CLONE3_L3_0, 1);
  record_task_groups(1, 3, 0);
  send_sigcode(busy_task, SIGCODE_CLONE3_L3_1, 5);
  record_task_groups(5, 3, 1);
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
    // TRACE_DEBUG("pid=%d, eligible=%d", p->pid,
    // ksym.entity_eligible(se->parent->cfs_rq, se->parent));
    if (ineligible_task == NULL &&
        ksym.entity_eligible(se->parent->cfs_rq, se->parent) == 0 &&
        ksym.entity_eligible(se->cfs_rq, se) == 1) {
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

static struct task_struct *get_task_by_cpu(int cpu) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == cpu && p->on_cpu == 1)
      return p;
  }
  return NULL;
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

  while (1) {
    find_not_eligible_tg();
    if (ineligible_tg_se != NULL) {
      TRACE_INFO("Found not eligible task group");
      sleep_all_tasks_in_ineligible_tg();
      break;
    }
    call_tick_once();
  }

  call_tick_once();

  while (1) {
    struct task_struct *p = get_task_by_cpu(cpu_of_ineligible_task);
    if (p != NULL) {
      TRACE_INFO("Cloning task to l2_1 from %d", p->pid);
      send_sigcode(p, SIGCODE_CLONE3_L2_1, 1);
      record_task_groups(1, 2, 1);
      // cpu_of_ineligible_task = -1;
      break;
    }
    call_tick_once();
  }

  while (1) {
    if (ineligible_tg_se->sched_delayed == 0) {
      TRACE_INFO("Ineligible task group is cleared at cpu: %d",
                 cpu_of_ineligible_task);
      break;
    }
    call_tick_once();
  }

  for (int i = 0; i < 60; i++) {
    call_tick_once();
  }
}

struct controller_ops controller_bbce3de = {
    .name = "bbce3de",
    .init = controller_init,
    .body = controller_body,
};
