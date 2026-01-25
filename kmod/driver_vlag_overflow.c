#include "driver.h"
#include "internal.h"

/*
 * Reproducer for commit 1560d1f6eb6b398bddd80c16676776c0325fe5fe
 * "sched/eevdf: Prevent vlag from going out of bounds in reweight_eevdf()"
 *
 * Bug: In reweight_eevdf(), vlag is computed without clamping. When multiplied
 * by old_weight, the s64 can overflow, corrupting vruntime.
 *
 * Strategy: Create a cgroup scheduling entity with artificially large vlag,
 * then trigger a weight change to cause overflow in reweight_eevdf().
 */

static struct task_struct *task0;
static struct task_struct *task1;

static void setup(void) {
  kstep_cgroup_create_pinned("g0", "1");
  kstep_cgroup_create_pinned("g1", "1");
  kstep_cgroup_set_weight("g0", 10000);
  kstep_cgroup_set_weight("g1", 100);

  task0 = kstep_task_create();
  task1 = kstep_task_create();
  kstep_cgroup_add_task("g0", task0->pid);
  kstep_cgroup_add_task("g1", task1->pid);
}

static void run(void) {
  struct sched_entity *se = task0->se.parent;
  struct cfs_rq *cfs_rq = se->cfs_rq;

  kstep_task_wakeup(task0);
  kstep_task_wakeup(task1);
  kstep_tick_repeat(10);

  u64 avruntime = ksym.avg_vruntime(cfs_rq);
  unsigned long weight = se->load.weight;

  // Create large negative vlag that will overflow when multiplied by weight
  // vlag = avruntime - vruntime, so set vruntime > avruntime
  s64 overflow_threshold = S64_MIN / (s64)weight;
  s64 target_vlag = overflow_threshold / 2;  // Large negative vlag
  se->vruntime = avruntime - target_vlag;

  u64 vruntime_before = se->vruntime;
  s64 vlag_before = (s64)(avruntime - se->vruntime);
  TRACE_INFO("vlag=%lld weight=%lu", vlag_before, weight);

  // Trigger reweight - this is where overflow happens in buggy kernel
  kstep_cgroup_set_weight("g0", 100);

  s64 vruntime_change = (s64)(se->vruntime - vruntime_before);
  s64 expected_max = 600000000LL;  // 600ms (based on clamped vlag * weight ratio)

  TRACE_INFO("vruntime_change=%lld ns", vruntime_change);

  if (vruntime_change > expected_max || vruntime_change < -expected_max) {
    TRACE_INFO("BUG: overflow detected (change=%lld sec)",
               vruntime_change / 1000000000LL);
  } else {
    TRACE_INFO("OK: vruntime change bounded (%lld ms)", vruntime_change / 1000000);
  }

  kstep_tick_repeat(5);
}

struct kstep_driver vlag_overflow = {
    .name = "vlag_overflow",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_tasks = true,
    .print_rq = true,
};
