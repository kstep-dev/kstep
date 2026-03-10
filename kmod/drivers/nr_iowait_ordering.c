// https://github.com/torvalds/linux/commit/ec618b84f6e1
//
// Bug: In try_to_wake_up(), nr_iowait is decremented early (before on_cpu
// ordering). When ttwu_queue_wakelist() fires before p->on_cpu==0, the
// decrement races with schedule()'s increment, leading to a transiently
// negative (or missed) nr_iowait and dodgy IO-wait statistics.
//
// Fix: Move the nr_iowait decrement from try_to_wake_up() into
// ttwu_do_activate() (for non-migrated wakeups) and after select_task_rq()
// (for migrated wakeups), ensuring proper ordering.
//
// Test: Directly call ttwu_do_activate() on a deactivated task with
// in_iowait=1 and check whether nr_iowait is decremented.
// On the buggy kernel, ttwu_do_activate has no iowait handling (the
// decrement lives in ttwu instead), so nr_iowait stays unchanged.
// On the fixed kernel, ttwu_do_activate decrements nr_iowait for
// non-migrated wakeups, so nr_iowait drops by 1.

#include "internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 9, 0)

typedef void (*ttwu_do_activate_fn_t)(struct rq *, struct task_struct *, int,
                                      struct rq_flags *);
typedef void (*deactivate_fn_t)(struct rq *, struct task_struct *, int);
typedef void (*update_rq_clock_fn_t)(struct rq *);

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  struct rq *rq = cpu_rq(1);
  struct rq_flags rf;

  ttwu_do_activate_fn_t ttwu_activate =
      (ttwu_do_activate_fn_t)kstep_ksym_lookup("ttwu_do_activate");
  deactivate_fn_t deact =
      (deactivate_fn_t)kstep_ksym_lookup("deactivate_task");
  update_rq_clock_fn_t upd_clock =
      (update_rq_clock_fn_t)kstep_ksym_lookup("update_rq_clock");

  if (!ttwu_activate || !deact || !upd_clock) {
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

  // Step 1: Deactivate the task (simulate schedule()'s dequeue)
  rq_lock(rq, &rf);
  upd_clock(rq);
  task->state = TASK_UNINTERRUPTIBLE;
  deact(rq, task, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
  rq_unlock(rq, &rf);

  // Step 2: Set in_iowait to simulate IO wait state
  task->in_iowait = 1;
  // Prevent sched_contributes_to_load from affecting nr_uninterruptible
  task->sched_contributes_to_load = 0;

  // Step 3: Set nr_iowait to a known value
  atomic_set(&rq->nr_iowait, 5);
  TRACE_INFO("nr_iowait set to %d before ttwu_do_activate",
             atomic_read(&rq->nr_iowait));

  // Step 4: Call ttwu_do_activate with wake_flags=0 (no WF_MIGRATED)
  // On buggy kernel: no iowait handling → nr_iowait stays 5
  // On fixed kernel: decrements nr_iowait → nr_iowait becomes 4
  rq_lock(rq, &rf);
  upd_clock(rq);
  ttwu_activate(rq, task, 0, &rf);
  rq_unlock(rq, &rf);

  int nr_iowait_after = atomic_read(&rq->nr_iowait);
  TRACE_INFO("nr_iowait after ttwu_do_activate: %d", nr_iowait_after);

  // On BUGGY kernel:
  //   ttwu_do_activate has no in_iowait handling.
  //   nr_iowait stays at 5. The decrement would normally happen too early
  //   in try_to_wake_up(), creating a race with schedule()'s increment.
  //
  // On FIXED kernel:
  //   ttwu_do_activate properly decrements nr_iowait for non-migrated tasks.
  //   nr_iowait becomes 4. The decrement now happens at the right time.

  if (nr_iowait_after == 5) {
    kstep_fail("ttwu_do_activate did not handle iowait (nr_iowait=%d): "
               "decrement is misplaced in ttwu, race with schedule possible",
               nr_iowait_after);
  } else if (nr_iowait_after == 4) {
    kstep_pass("ttwu_do_activate properly decrements nr_iowait (%d)",
               nr_iowait_after);
  } else {
    kstep_fail("unexpected nr_iowait=%d", nr_iowait_after);
  }

  // Clean up
  task->in_iowait = 0;
  atomic_set(&rq->nr_iowait, 0);
  kstep_tick_repeat(5);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "nr_iowait_ordering",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
