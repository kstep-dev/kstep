// https://github.com/torvalds/linux/commit/1560d1f6eb6b398bddd80c16676776c0325fe5fe

#include "driver.h"
#include "internal.h"
#include <linux/math.h>
#include <linux/math64.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)

/*
 * Reproducer strategy (minimal version):
 *
 * 1) Build a nested cgroup path (vlag/group/heavy) so we exercise a
 *    non-leaf group sched_entity.
 * 2) Reduce sched_base_slice to tighten the lag bound (entity_lag limit).
 * 3) Keep two tiny migrators in a sibling subgroup and bounce them across CPUs.
 *    This adds enough placement jitter without directly writing
 *    scheduler internals.
 *    With A1a at cpu.weight=10000 and sibling A1b at cpu.weight=1,
 *    repeated enqueue/dequeue across CPUs perturbs avg_vruntime far more than
 *    A1a's own vruntime advance, so raw_vlag can grow past entity_lag bounds.
 * 4) Poll group lag and reweight immediately at first out-of-bounds point.
 *
 * On buggy kernels this reweight uses raw vlag; on fixed kernels the value
 * is clamped first in reweight_eevdf().
 */

static struct task_struct *target_task;
static struct task_struct *mover_task0;
static struct task_struct *mover_task1;

static u64 mul_u64_sat(u64 a, u64 b) {
  if (a && b > (~0ULL) / a)
    return ~0ULL;
  return a * b;
}

static s64 lag_limit_est(struct sched_entity *se) {
  unsigned long w = scale_load_down(se->load.weight);

  if (!w)
    w = 1;

  /*
   * entity_lag() limit is calc_delta_fair(delta, se), which is approximately
   * delta * NICE_0_LOAD / load. NICE_0_LOAD is 1024 on this build.
   */
  return (s64)div64_u64(max_t(u64, 2 * se->slice, TICK_NSEC) * 1024ULL, w);
}

static void mover_round(int cpu_hi, int cpu_lo) {
  /*
   * One round = migrate, swap, then restore. The third move returns both
   * movers to a fixed phase so every loop starts from the same placement.
   */
  kstep_task_pin(mover_task0, cpu_hi, cpu_hi);
  kstep_task_pin(mover_task1, cpu_lo, cpu_lo);
  kstep_tick_repeat(2);
  kstep_task_pin(mover_task0, cpu_lo, cpu_lo);
  kstep_task_pin(mover_task1, cpu_hi, cpu_hi);
  kstep_tick_repeat(2);
  kstep_task_pin(mover_task0, cpu_hi, cpu_hi);
  kstep_task_pin(mover_task1, cpu_lo, cpu_lo);
  kstep_tick_repeat(2);
}

static void setup(void) {
  KSYM_IMPORT(sysctl_sched_base_slice);
  /*
   * sched_base_slice (ns) is the base CFS/EEVDF service slice used when
   * computing per-entity slice/deadline. entity_lag() bounds lag with:
   *   lag_limit ~= calc_delta_fair(max(2 * se->slice, TICK_NSEC), se)
   * so reducing base_slice shrinks the lag bound. That makes the same runtime
   * phase offset reach raw_vlag/limit > 1.0 with less perturbation.
   */
  TRACE_INFO("Setting sysctl_sched_base_slice from %u to 10000",
             *KSYM_sysctl_sched_base_slice);
  *KSYM_sysctl_sched_base_slice = 10000;

  if (num_online_cpus() != 3)
    panic("vlag_overflow expects exactly 3 CPUs, got %d", num_online_cpus());

  kstep_cgroup_create("vlag");
  kstep_cgroup_create("vlag/group");
  kstep_cgroup_create("vlag/group/heavy");
  kstep_cgroup_create("vlag/group/movers");

  kstep_cgroup_set_weight("vlag", 10000);
  kstep_cgroup_set_weight("vlag/group", 10000);
  kstep_cgroup_set_weight("vlag/group/heavy", 10000);
  kstep_cgroup_set_weight("vlag/group/movers", 1);

  target_task = kstep_task_create();
  mover_task0 = kstep_task_create();
  mover_task1 = kstep_task_create();

  kstep_cgroup_add_task("vlag/group/heavy", target_task->pid);
  kstep_cgroup_add_task("vlag/group/movers", mover_task0->pid);
  kstep_cgroup_add_task("vlag/group/movers", mover_task1->pid);
}

static void run(void) {
  KSYM_IMPORT(avg_vruntime);
  const s64 min_over = 512;

  kstep_task_pin(target_task, 1, 1);
  kstep_task_pin(mover_task0, 1, 1);
  kstep_task_pin(mover_task1, 2, 2);
  kstep_tick_repeat(10);

  for (int i = 0;; i++) {
    if (i > 10000) {
      TRACE_INFO("no trigger in %d iterations", i);
      return;
    }

    mover_round(2, 1);

    struct sched_entity *se = target_task->se.parent;
    if (!se)
      panic("target has no group entity");
    u64 avruntime = KSYM_avg_vruntime(cfs_rq_of(se));
    s64 raw_vlag = (s64)(avruntime - se->vruntime);
    s64 clamped_vlag = se->vlag;
    s64 abs_raw = abs(raw_vlag);
    s64 abs_clamped = abs(clamped_vlag);
    s64 limit_est = lag_limit_est(se);
    s64 over_by = abs_raw - limit_est;

    /*
     * Start reweight only when raw lag is clearly above the estimated
     * entity_lag bound; then classify whether buggy or fixed path was used.
     */
    if (se->on_rq && over_by >= min_over) {
      u64 old_weight = se->load.weight;
      u64 before_vruntime = se->vruntime;
      u64 before_abs_raw = abs_raw;
      u64 before_abs_clamped = (u64)limit_est;
      u64 ratio;
      u64 expected_bug;
      u64 expected_fix;
      u64 observed_after;
      bool buggy_path;

      /*
       * Reweight while raw_vlag is out of bounds.
       * Buggy:  uses raw_vlag in reweight_eevdf().
       * Fixed:  uses entity_lag() (i.e. clamped vlag) there.
       */
      TRACE_INFO(
          "trigger_before_hot: avruntime=%llu vruntime=%llu raw_vlag=%lld "
          "clamped_vlag=%lld abs_raw=%lld abs_clamped=%lld "
          "limit_est=%lld over_by=%lld weight=%lu",
          avruntime, se->vruntime, raw_vlag, clamped_vlag, abs_raw, abs_clamped,
          limit_est, over_by, se->load.weight);
      kstep_cgroup_set_weight("vlag/group/heavy", 1);

      avruntime = KSYM_avg_vruntime(cfs_rq_of(se));
      raw_vlag = (s64)(avruntime - se->vruntime);
      clamped_vlag = se->vlag;
      abs_raw = abs(raw_vlag);
      abs_clamped = abs(clamped_vlag);
      if (!se->load.weight)
        panic("new weight is zero");

      ratio = div64_u64(old_weight, se->load.weight);
      if (!ratio)
        ratio = 1;
      expected_bug = mul_u64_sat(before_abs_raw, ratio);
      expected_fix = mul_u64_sat(before_abs_clamped, ratio);
      observed_after = abs_raw;
      buggy_path = abs_diff(observed_after, expected_bug) <
                   abs_diff(observed_after, expected_fix);

      TRACE_INFO("trigger_after_hot: avruntime=%llu vruntime=%llu "
                 "raw_vlag=%lld clamped_vlag=%lld weight=%lu",
                 avruntime, se->vruntime, raw_vlag, clamped_vlag, se->load.weight);
      TRACE_INFO("path_check: ratio=%llu before_abs_raw=%llu "
                 "before_abs_clamped=%llu expected_bug=%llu expected_fix=%llu "
                 "observed_after=%llu buggy_path=%d",
                 ratio, before_abs_raw, before_abs_clamped, expected_bug,
                 expected_fix, observed_after, buggy_path);
      TRACE_INFO("impact: vruntime_jump=%lld",
                 (s64)(se->vruntime - before_vruntime));
      return;
    }
    kstep_tick();
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "vlag_overflow",
    .setup = setup,
    .run = run,
    .step_interval_us = 50,
};
