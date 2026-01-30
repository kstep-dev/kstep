#include "driver.h"
#include "internal.h"

/*
 * Reproducer for Linux commit 0258bdfaff5b ("sched/fair: Fix unfairness caused
 * by missing load decay").
 *
 * An idle task is attached to a cgroup restricted to CPU2, then the cpuset is
 * changed to CPU1 before the task ever runs. The missing leaf list entry causes
 * the old load to stick on the original cfs_rq, so its load_avg does not decay.
 * After some ticks we log the per-CPU cfs_rq load_avg to observe the stale load.
 */

static struct task_struct *idle_task;
static struct task_struct *g1_task;
static struct task_struct *g2_task;

static void setup(void) {
  kstep_cgroup_create("g1");
  kstep_cgroup_create("g2");
  kstep_cgroup_set_weight("g1", 100);
  kstep_cgroup_set_weight("g2", 100);

  /* Start with g1 limited to CPU2 so the initial attach lands there. */
  kstep_cgroup_set_cpuset("g1", "2");
  kstep_cgroup_set_cpuset("g2", "1");

  idle_task = kstep_task_create();
  kstep_cgroup_add_task("g1", idle_task->pid);

  g1_task = kstep_task_create();
  g2_task = kstep_task_create();
}

static void run(void) {
  /* Move g1 to CPU1 before the idle task ever runs. */
  kstep_cgroup_set_cpuset("g1", "1");
  kstep_tick_repeat(10);

  struct task_group *tg = task_group(idle_task);
  struct cfs_rq *rq1 = tg->cfs_rq[1];
  struct cfs_rq *rq2 = tg->cfs_rq[2];

  TRACE_INFO("g1 load_avg after move: cpu1=%llu cpu2=%llu", rq1->avg.load_avg,
             rq2->avg.load_avg);

  /* Allow PELT decay to run; on the buggy kernel load_avg stays elevated. */
  kstep_tick_repeat(200);
  TRACE_INFO("g1 load_avg after decay: cpu1=%llu cpu2=%llu", rq1->avg.load_avg,
             rq2->avg.load_avg);

  /* Run two busy tasks in separate groups on CPU1 to expose unfairness. */
  kstep_cgroup_add_task("g1", g1_task->pid);
  kstep_cgroup_add_task("g2", g2_task->pid);
  kstep_task_wakeup(g1_task);
  kstep_task_wakeup(g2_task);

  kstep_tick_repeat(400);
  TRACE_INFO("sum_exec_runtime ns: g1=%lld g2=%lld",
             g1_task->se.sum_exec_runtime, g2_task->se.sum_exec_runtime);
}

struct kstep_driver missing_load_decay = {
    .name = "missing_load_decay",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
    .print_tasks = true,
};
