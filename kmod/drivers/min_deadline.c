// https://github.com/torvalds/linux/commit/8dafa9d0eb1a1550a0f4d462db9354161bc51e0c

/*
 * Reproduce: sched/eevdf: Fix min_deadline heap integrity (8dafa9d0eb1a)
 *
 * Bug: reweight_entity() scales se->deadline when weight changes for an on_rq
 * entity, but fails to call min_deadline_cb_propagate() to maintain the
 * augmented RB-tree heap property. This corrupts the min_deadline values that
 * pick_eevdf() relies on to find the best eligible entity.
 *
 * Trigger: Change a cgroup's cpu.weight while its group SE is in the tree
 * (on_rq but not curr). This calls reweight_entity() which modifies the
 * deadline without propagating the min_deadline change upward.
 */

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 6, 0)

#define NUM_ROOT_TASKS 2
#define NUM_TASKS (NUM_ROOT_TASKS + 1)

static struct task_struct *tasks[NUM_TASKS];

static inline struct sched_entity *node_to_se(struct rb_node *node)
{
  return rb_entry(node, struct sched_entity, run_node);
}

// Verify min_deadline heap property at every node.
// Each node's min_deadline must equal min(self.deadline, left.min_deadline,
// right.min_deadline). Returns false if any node is corrupted.
static bool check_min_deadline_node(struct rb_node *node)
{
  if (!node)
    return true;

  struct sched_entity *se = node_to_se(node);
  u64 expected = se->deadline;

  if (node->rb_left) {
    struct sched_entity *left = node_to_se(node->rb_left);
    if ((s64)(left->min_deadline - expected) < 0)
      expected = left->min_deadline;
  }
  if (node->rb_right) {
    struct sched_entity *right = node_to_se(node->rb_right);
    if ((s64)(right->min_deadline - expected) < 0)
      expected = right->min_deadline;
  }

  if (se->min_deadline != expected) {
    TRACE_INFO("CORRUPTED: deadline=%lld min_deadline=%lld expected=%lld",
               (s64)se->deadline, (s64)se->min_deadline, (s64)expected);
    return false;
  }

  return check_min_deadline_node(node->rb_left) &&
         check_min_deadline_node(node->rb_right);
}

static bool check_cfs_rq_min_deadline(struct cfs_rq *cfs_rq)
{
  return check_min_deadline_node(cfs_rq->tasks_timeline.rb_root.rb_node);
}

static void setup(void)
{
  for (int i = 0; i < NUM_TASKS; i++)
    tasks[i] = kstep_task_create();
  kstep_cgroup_create("g0");
}

static void run(void)
{
  // Pin all tasks to CPU 1 and wake them up
  for (int i = 0; i < NUM_TASKS; i++) {
    kstep_task_pin(tasks[i], 1, 1);
    kstep_task_wakeup(tasks[i]);
  }

  // Move the last task into cgroup g0
  kstep_cgroup_add_task("g0", tasks[NUM_TASKS - 1]->pid);

  // Tick to establish vruntimes and deadlines
  kstep_tick_repeat(10);

  // Get the group SE via the cgroup task's parent pointer
  struct sched_entity *group_se = tasks[NUM_TASKS - 1]->se.parent;
  struct cfs_rq *parent_cfs = group_se->cfs_rq;

  TRACE_INFO("group_se: on_rq=%d is_curr=%d deadline=%lld min_deadline=%lld",
             group_se->on_rq, parent_cfs->curr == group_se,
             (s64)group_se->deadline, (s64)group_se->min_deadline);

  // Verify tree is intact before any weight change
  bool ok_before = check_cfs_rq_min_deadline(parent_cfs);
  TRACE_INFO("Before reweight: tree %s", ok_before ? "OK" : "CORRUPTED");

  // Try multiple weight changes with ticks in between.
  // The bug triggers when the group SE is NOT curr (i.e., it's in the tree).
  // Ticking between changes rotates which entity is curr.
  int weights[] = {10000, 1, 5000, 50, 10000};
  bool corrupted = false;

  for (int w = 0; w < ARRAY_SIZE(weights); w++) {
    TRACE_INFO("Setting weight=%d, group_se is_curr=%d", weights[w],
               parent_cfs->curr == group_se);

    kstep_cgroup_set_weight("g0", weights[w]);
    kstep_sleep();

    if (!check_cfs_rq_min_deadline(parent_cfs)) {
      TRACE_INFO("min_deadline corrupted after weight=%d", weights[w]);
      corrupted = true;
      break;
    }
    kstep_tick();
  }

  if (corrupted)
    kstep_fail("min_deadline heap corrupted after cgroup reweight");
  else
    kstep_pass("min_deadline heap intact");
}

KSTEP_DRIVER_DEFINE{
    .name = "min_deadline",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
