// https://github.com/torvalds/linux/commit/dbfb089d360b
//
// Bug: deactivate_task() reads p->state via task_contributes_to_load() to
// decide whether to increment rq->nr_uninterruptible. If ttwu() on another
// CPU changes p->state to TASK_WAKING between deactivate_task's on_rq=0
// write and its task_contributes_to_load() read, the increment is lost,
// causing nr_uninterruptible to drift and loadavg accounting errors.
//
// Fix: Move nr_uninterruptible accounting from deactivate_task/activate_task
// into __schedule() (using a captured state snapshot) and ttwu_do_activate().
//
// Test: Manually call deactivate_task() with TASK_UNINTERRUPTIBLE and
// TASK_WAKING states. On the buggy kernel, the nr_uninterruptible delta
// depends on the live p->state (sensitive to concurrent modification by
// ttwu). On the fixed kernel, deactivate_task never touches
// nr_uninterruptible (immune to the race).

#include "internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

typedef void (*deactivate_fn_t)(struct rq *, struct task_struct *, int);
typedef void (*activate_fn_t)(struct rq *, struct task_struct *, int);
typedef void (*update_rq_clock_fn_t)(struct rq *);

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  struct rq *rq = cpu_rq(1);
  struct rq_flags rf;

  deactivate_fn_t deact = (deactivate_fn_t)kstep_ksym_lookup("deactivate_task");
  activate_fn_t act = (activate_fn_t)kstep_ksym_lookup("activate_task");
  update_rq_clock_fn_t upd_clock =
      (update_rq_clock_fn_t)kstep_ksym_lookup("update_rq_clock");

  if (!deact || !act || !upd_clock) {
    kstep_fail("cannot find required symbols");
    return;
  }

  kstep_tick_repeat(5);

  TRACE_INFO("task pid=%d cpu=%d on_rq=%d state=0x%lx",
             task->pid, task_cpu(task), task->on_rq, task->state);

  if (task->on_rq != TASK_ON_RQ_QUEUED) {
    kstep_fail("task not on rq, cannot test");
    return;
  }

  long baseline = rq->nr_uninterruptible;
  TRACE_INFO("baseline nr_uninterruptible=%ld", baseline);

  // Test 1: Deactivate with TASK_UNINTERRUPTIBLE state
  rq_lock(rq, &rf);
  upd_clock(rq);
  task->state = TASK_UNINTERRUPTIBLE;
  deact(rq, task, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
  long after_deact_unint = rq->nr_uninterruptible;
  task->state = TASK_RUNNING;
  act(rq, task, ENQUEUE_WAKEUP | ENQUEUE_NOCLOCK);
  rq_unlock(rq, &rf);

  long delta_unint = after_deact_unint - baseline;
  TRACE_INFO("deactivate(TASK_UNINTERRUPTIBLE): delta=%ld", delta_unint);

  // Test 2: Deactivate with TASK_WAKING state (simulates race outcome
  // where ttwu() changed state before deactivate_task reads it)
  long before_waking = rq->nr_uninterruptible;
  rq_lock(rq, &rf);
  upd_clock(rq);
  task->state = TASK_WAKING;
  deact(rq, task, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
  long after_deact_waking = rq->nr_uninterruptible;
  task->state = TASK_RUNNING;
  act(rq, task, ENQUEUE_WAKEUP | ENQUEUE_NOCLOCK);
  rq_unlock(rq, &rf);

  long delta_waking = after_deact_waking - before_waking;
  TRACE_INFO("deactivate(TASK_WAKING): delta=%ld", delta_waking);

  // On buggy kernel:
  //   delta_unint = +1 (task_contributes_to_load reads TASK_UNINTERRUPTIBLE)
  //   delta_waking = 0 (task_contributes_to_load reads TASK_WAKING -> false)
  //   The difference shows deactivate_task depends on live p->state,
  //   so a concurrent ttwu() changing state causes nr_uninterruptible drift.
  //
  // On fixed kernel:
  //   delta_unint = 0 (deactivate_task has no nr_uninterruptible accounting)
  //   delta_waking = 0 (same)
  //   deactivate_task is immune to state changes.

  TRACE_INFO("final nr_uninterruptible=%ld (baseline was %ld)",
             rq->nr_uninterruptible, baseline);

  if (delta_unint != delta_waking) {
    kstep_fail("deactivate_task depends on live p->state: "
               "UNINTERRUPTIBLE delta=%ld, WAKING delta=%ld "
               "(race causes nr_uninterruptible drift)",
               delta_unint, delta_waking);
  } else {
    kstep_pass("deactivate_task accounting independent of p->state "
               "(delta=%ld for both)", delta_unint);
  }

  kstep_tick_repeat(3);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "loadavg_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
