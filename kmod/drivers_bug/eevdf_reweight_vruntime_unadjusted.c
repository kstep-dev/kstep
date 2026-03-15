// Bug: place_entity() double-counts curr weight during reweight,
// breaking lag preservation.
//
// In v6.19's reweight_entity(), when se == cfs_rq->curr:
// 1. avg_vruntime() uses NEW weight with OLD vruntime -> wrong V
// 2. place_entity() inflation adds both curr->load.weight and
//    se->load.weight (same entity) -> factor (W+2w')/(W+w')
//
// Reference: eab03c23c2a1 (original vruntime reweight fix)
// Regression: 6d71a9c61604 (replaced reweight_eevdf with place_entity)

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

typedef void(reweight_entity_fn_t)(struct cfs_rq *, struct sched_entity *,
                                   unsigned long);
KSYM_IMPORT_TYPED(reweight_entity_fn_t, reweight_entity);
KSYM_IMPORT(avg_vruntime);

static struct task_struct *task_a;
static struct task_struct *task_b;

static void setup(void) {
  task_a = kstep_task_create();
  task_b = kstep_task_create();
}

static void run(void) {
  struct rq *rq = cpu_rq(1);
  struct cfs_rq *cfs_rq = &rq->cfs;

  if (!KSYM_reweight_entity || !KSYM_avg_vruntime) {
    kstep_fail("Symbol resolution failed: reweight_entity=%px "
               "avg_vruntime=%px",
               KSYM_reweight_entity, KSYM_avg_vruntime);
    return;
  }

  // Give task_b a different weight to create asymmetric vruntimes.
  // task_a: nice 0 (w=1024), task_b: nice 5 (w=335)
  kstep_task_set_prio(task_b, 5);

  kstep_task_wakeup(task_a);
  kstep_task_wakeup(task_b);

  // Run to establish vruntimes; with different weights, vlags diverge.
  kstep_tick_repeat(20);

  struct sched_entity *se = &task_a->se;

  // Tick until task_a is curr on CPU 1
  for (int i = 0; i < 40; i++) {
    if (cfs_rq->curr == se)
      break;
    kstep_tick();
  }
  if (cfs_rq->curr != se) {
    kstep_fail("Could not make task_a curr");
    kstep_tick_repeat(5);
    return;
  }

  TRACE_INFO("task_a pid=%d is curr, on_rq=%d", task_a->pid, se->on_rq);

  // Lock the rq to safely call reweight_entity
  raw_spin_lock(&rq->__lock);
  rq->clock_update_flags |= RQCF_UPDATED;

  // Capture pre-reweight state
  u64 V_before = KSYM_avg_vruntime(cfs_rq);
  u64 v_before = se->vruntime;
  s64 vlag_before = (s64)(V_before - v_before);
  unsigned long w_old = se->load.weight;

  TRACE_INFO("PRE:  V=%llu v=%llu vlag=%lld w_scaled=%lu w=%lu",
             V_before, v_before, vlag_before, w_old,
             scale_load_down(w_old));

  // If vlag is zero, we cannot distinguish buggy from correct. Try nudging
  // the vruntime to create a meaningful lag signal.
  if (vlag_before == 0) {
    // Shift v backwards to synthesize positive vlag. This is safe because
    // we immediately call reweight_entity which recomputes everything.
    se->vruntime -= 500000; // 0.5ms shift
    V_before = KSYM_avg_vruntime(cfs_rq);
    v_before = se->vruntime;
    vlag_before = (s64)(V_before - v_before);
    TRACE_INFO("NUDGED: V=%llu v=%llu vlag=%lld", V_before, v_before,
               vlag_before);
  }

  // Reweight from nice 0 (1024) to nice 5 (335)
  unsigned long w_new = scale_load(335);
  TRACE_INFO("Reweighting curr: %lu -> %lu", w_old, w_new);

  KSYM_reweight_entity(cfs_rq, se, w_new);

  // Capture post-reweight state
  u64 V_after = KSYM_avg_vruntime(cfs_rq);
  u64 v_after = se->vruntime;
  s64 vlag_after = (s64)(V_after - v_after);
  unsigned long w_after = se->load.weight;

  raw_spin_unlock(&rq->__lock);

  TRACE_INFO("POST: V=%llu v=%llu vlag=%lld w=%lu",
             V_after, v_after, vlag_after, scale_load_down(w_after));

  // Check 1: V preservation (Corollary #2)
  s64 V_delta = (s64)(V_after - V_before);
  TRACE_INFO("V delta: %lld (should be ~0)", V_delta);

  // Check 2: vruntime correctness
  // Correct: v' = V_old - (V_old - v_old) * w_old / w_new
  s64 expected_vlag = div_s64(vlag_before * (s64)w_old, (s64)w_new);
  u64 expected_v = V_before - expected_vlag;
  s64 v_error = (s64)(v_after - expected_v);

  TRACE_INFO("Expected v=%llu actual=%llu error=%lld", expected_v, v_after,
             v_error);

  s64 threshold = 1000; // 1us
  bool v_bad = v_error > threshold || v_error < -threshold;
  bool V_bad = V_delta > threshold || V_delta < -threshold;

  if (v_bad || V_bad) {
    kstep_fail("Reweight broke lag: V_delta=%lld v_error=%lld",
               V_delta, v_error);
  } else {
    kstep_pass("Lag preserved: V_delta=%lld v_error=%lld", V_delta, v_error);
  }

  kstep_tick_repeat(10);
}

KSTEP_DRIVER_DEFINE{
    .name = "eevdf_reweight_vruntime_unadjusted",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
