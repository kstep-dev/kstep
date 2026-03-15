/*
 * Reproducer for: Refactored reweight_entity() double-counts curr weight
 * in place_entity() inflation.
 *
 * Commit 6d71a9c61604 replaced reweight_eevdf() with update_entity_lag() +
 * place_entity(). When se == cfs_rq->curr, place_entity()'s lag inflation
 * formula counts curr's weight twice in the numerator:
 *   load = avg_load + w_curr  (curr check)
 *   inflated = vlag * (load + w_se) / load
 * Since se == curr, w_se == w_curr, giving (W + 2w)/(W + w) instead of
 * the correct (W + w)/W.
 *
 * We trigger this by changing a cgroup's weight while its group SE is curr
 * on the parent cfs_rq, then checking that weighted lag is preserved.
 */

#include <linux/version.h>
#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

KSYM_IMPORT(avg_vruntime);

static struct task_struct *task_a;
static struct task_struct *task_b;

static void setup(void)
{
  kstep_cgroup_create("g0");
  kstep_cgroup_create("g1");

  task_a = kstep_task_create();
  task_b = kstep_task_create();

  kstep_cgroup_add_task("g0", task_a->pid);
  kstep_cgroup_add_task("g1", task_b->pid);
}

static void run(void)
{
  struct rq *rq = cpu_rq(1);
  struct cfs_rq *parent_cfs = &rq->cfs;
  struct sched_entity *se_g0 = task_a->se.parent;
  struct sched_entity *se_g1 = task_b->se.parent;

  kstep_task_wakeup(task_a);
  kstep_task_wakeup(task_b);

  /* Tick to build up lag on the group SEs */
  kstep_tick_repeat(50);

  /* Identify which group SE is curr */
  struct sched_entity *curr_se = parent_cfs->curr;
  if (!curr_se) {
    kstep_fail("no curr on parent cfs_rq");
    return;
  }

  bool g0_is_curr = (curr_se == se_g0);
  struct sched_entity *se = g0_is_curr ? se_g0 : se_g1;
  const char *cg_name = g0_is_curr ? "g0" : "g1";

  TRACE_INFO("Group SE for %s is curr on root cfs_rq", cg_name);

  /* Record state before reweight */
  u64 V_before = KSYM_avg_vruntime(parent_cfs);
  u64 v_before = se->vruntime;
  unsigned long w_before = se->load.weight;
  s64 vlag_before = (s64)(V_before - v_before);
  s64 wlag_before = (s64)w_before * vlag_before;

  TRACE_INFO("BEFORE: V=%llu v=%llu w=%lu vlag=%lld wlag=%lld",
             V_before, v_before, w_before, vlag_before, wlag_before);

  /* Also record the other SE's state for reference */
  struct sched_entity *other = g0_is_curr ? se_g1 : se_g0;
  TRACE_INFO("OTHER SE: v=%llu w=%lu on_rq=%d",
             other->vruntime, other->load.weight, other->on_rq);
  TRACE_INFO("cfs_rq: avg_load=%llu avg_vruntime=%lld nr_queued=%u",
             (unsigned long long)parent_cfs->avg_load,
             (long long)parent_cfs->avg_vruntime,
             parent_cfs->nr_queued);

  /*
   * Change the cgroup weight to trigger reweight_entity().
   * Default cpu.weight is 100; change to 500 for a significant
   * weight ratio change.
   */
  kstep_cgroup_set_weight(cg_name, 500);

  /* Record state after reweight */
  u64 V_after = KSYM_avg_vruntime(parent_cfs);
  u64 v_after = se->vruntime;
  unsigned long w_after = se->load.weight;
  s64 vlag_after = (s64)(V_after - v_after);
  s64 wlag_after = (s64)w_after * vlag_after;

  TRACE_INFO("AFTER: V=%llu v=%llu w=%lu vlag=%lld wlag=%lld",
             V_after, v_after, w_after, vlag_after, wlag_after);
  TRACE_INFO("cfs_rq AFTER: avg_load=%llu avg_vruntime=%lld",
             (unsigned long long)parent_cfs->avg_load,
             (long long)parent_cfs->avg_vruntime);

  /* Check if weighted lag is preserved */
  if (wlag_before == 0) {
    TRACE_INFO("wlag_before is 0; trying with more ticks");
    kstep_tick_repeat(30);

    curr_se = parent_cfs->curr;
    if (!curr_se) {
      kstep_fail("no curr after extra ticks");
      return;
    }

    g0_is_curr = (curr_se == se_g0);
    se = g0_is_curr ? se_g0 : se_g1;
    cg_name = g0_is_curr ? "g0" : "g1";

    V_before = KSYM_avg_vruntime(parent_cfs);
    v_before = se->vruntime;
    w_before = se->load.weight;
    vlag_before = (s64)(V_before - v_before);
    wlag_before = (s64)w_before * vlag_before;

    TRACE_INFO("RETRY BEFORE: V=%llu v=%llu w=%lu vlag=%lld wlag=%lld",
               V_before, v_before, w_before, vlag_before, wlag_before);

    /* Change weight back to default for the retry */
    kstep_cgroup_set_weight(cg_name, 100);

    V_after = KSYM_avg_vruntime(parent_cfs);
    v_after = se->vruntime;
    w_after = se->load.weight;
    vlag_after = (s64)(V_after - v_after);
    wlag_after = (s64)w_after * vlag_after;

    TRACE_INFO("RETRY AFTER: V=%llu v=%llu w=%lu vlag=%lld wlag=%lld",
               V_after, v_after, w_after, vlag_after, wlag_after);
  }

  if (wlag_before == 0) {
    kstep_fail("weighted lag is 0, cannot measure error");
    kstep_tick_repeat(10);
    return;
  }

  s64 error = wlag_after - wlag_before;
  s64 abs_wlag = wlag_before < 0 ? -wlag_before : wlag_before;
  s64 abs_error = error < 0 ? -error : error;
  s64 error_pct = abs_error * 100 / abs_wlag;

  TRACE_INFO("Weighted lag error: %lld (%lld%%)", error, error_pct);

  if (error_pct > 5) {
    kstep_fail("weighted lag NOT preserved through reweight: "
               "before=%lld after=%lld error=%lld%%",
               wlag_before, wlag_after, error_pct);
  } else {
    kstep_pass("weighted lag preserved: "
               "before=%lld after=%lld error=%lld%%",
               wlag_before, wlag_after, error_pct);
  }

  kstep_tick_repeat(10);
}

static void on_tick_begin(void)
{
  kstep_output_curr_task();
}

KSTEP_DRIVER_DEFINE{
    .name = "eevdf_reweight_dequeue_avruntime",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};

#endif
