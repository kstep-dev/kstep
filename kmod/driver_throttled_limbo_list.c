// https://github.com/torvalds/linux/commit/956dfda6a70885f18c0f8236a461aa2bc4f556ad

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

/*
 * Reproducer for Linux commit 956dfda6a708 (fixed):
 * sched/fair: Prevent cfs_rq from being unthrottled with zero runtime_remaining
 *
 * Trigger the WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_limbo_list)) in
 * tg_throttle_down() by forcing a throttle during the unthrottle path.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)

static struct task_struct *t0;
static struct task_struct *t1;

static void *both_tasks_throttled(void) {
  if (t0 && t1 && t0->throttled && t1->throttled)
    return t0;
  return NULL;
}

static void setup(void) {
  /*
   * Hierarchy (cgroup v2):
   *   root
   *    \- A (quota)
   *       \- B (internal)
   *       |  \- C (quota enabled, initially empty)
   *       \- D (leaf, tasks start here)
   */
  kstep_cgroup_create("A");
  kstep_cgroup_create("A/B");
  kstep_cgroup_create("A/B/C");
  kstep_cgroup_create("A/D");

  /* A throttles quickly. period: 100ms, quota: 1ms (minimum accepted). */
  kstep_cgroup_write("A", "cpu.max", "1000 100000");

  /*
   * C has runtime enabled but no budget. On the buggy kernel, its cfs_rq can be
   * initialized unthrottled with runtime_remaining == 0.
   */
  /*
   * cpu.max doesn't accept 0 quota (EINVAL). Use 1us to get a tiny budget.
   * The buggy behavior we want is runtime_remaining==0 at init-time, which is
   * a kernel-internal state set by tg_set_cfs_bandwidth().
   */
  /* C has quota enabled with minimum accepted quota (1ms). */
  kstep_cgroup_write("A/B/C", "cpu.max", "1000 100000");

  t0 = kstep_task_create();
  t1 = kstep_task_create();

  /* Put tasks in a leaf cgroup under A. */
  kstep_cgroup_add_task("A/D", t0->pid);
  kstep_cgroup_add_task("A/D", t1->pid);
}

static void run(void) {
  kstep_task_wakeup(t0);
  kstep_task_wakeup(t1);

  /* Let tasks run long enough for A to become throttled. */
  kstep_tick_until(both_tasks_throttled);
  TRACE_INFO("Both tasks throttled; migrating to C while A is throttled");

  /* While A is throttled, migrate throttled tasks into C. */
  kstep_cgroup_add_task("A/B/C", t0->pid);
  kstep_cgroup_add_task("A/B/C", t1->pid);

  {
    struct task_group *tg = task_group(t0);
    struct cfs_rq *cfs_rq = tg->cfs_rq[1];
    struct cfs_bandwidth *cfs_b = &tg->cfs_bandwidth;
    unsigned long flags;
    TRACE_INFO(
        "C cfs_rq: throttled=%u throttle_count=%u runtime_remaining=%lld",
        cfs_rq->throttled, cfs_rq->throttle_count,
        (long long)cfs_rq->runtime_remaining);

    /*
     * Force the exact failure mode described in 956dfda6a708:
     * runtime_remaining==0 on an unthrottled cfs_rq, and no bandwidth runtime
     * available, so the first enqueue during unthrottle will throttle and hit
     * WARN_ON_ONCE(!list_empty(&cfs_rq->throttled_limbo_list)).
     */
    raw_spin_lock_irqsave(&cfs_b->lock, flags);
    cfs_b->runtime = 0;
    cfs_b->runtime_snap = 0;
    raw_spin_unlock_irqrestore(&cfs_b->lock, flags);
    TRACE_INFO("Forced C bandwidth runtime=0");
  }

  /* Give the migration/enqueue path a moment to run. */
  kstep_tick_repeat(10);

  /* Unthrottle A by disabling its quota. This walks tg_unthrottle_up() into C.
   */
  TRACE_INFO("Disabling A quota to trigger unthrottle path");
  kstep_cgroup_write("A", "cpu.max", "max 100000");

  /* Allow the unthrottle/enqueue/throttle sequence to run. */
  kstep_tick_repeat(20);

  TRACE_INFO("Done; check kernel log for WARN in tg_throttle_down()");
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "throttled_limbo_list",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
    .print_tasks = true,
};
