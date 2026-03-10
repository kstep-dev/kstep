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
// Test: Simulate the race window by deactivating a task with in_iowait=1,
// keeping on_cpu=1 (as if schedule() hasn't finished), then calling
// wake_up_process(). With on_cpu=1, ttwu takes the ttwu_queue_wakelist()
// path (deferred wakeup via IPI). On the buggy kernel, nr_iowait is
// decremented immediately (before the wakelist path), so we observe the
// decrement right after wake_up_process returns. On the fixed kernel, the
// decrement is deferred to ttwu_do_activate (when the wakelist is processed),
// so nr_iowait is unchanged after wake_up_process returns.

#include "internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 9, 0)

typedef void (*deactivate_fn_t)(struct rq *, struct task_struct *, int);
typedef void (*update_rq_clock_fn_t)(struct rq *);
typedef void (*sched_ttwu_pending_fn_t)(void *);

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  struct rq *rq = cpu_rq(1);
  struct rq_flags rf;

  deactivate_fn_t deact =
      (deactivate_fn_t)kstep_ksym_lookup("deactivate_task");
  update_rq_clock_fn_t upd_clock =
      (update_rq_clock_fn_t)kstep_ksym_lookup("update_rq_clock");
  sched_ttwu_pending_fn_t ttwu_pending =
      (sched_ttwu_pending_fn_t)kstep_ksym_lookup("sched_ttwu_pending");

  if (!deact || !upd_clock || !ttwu_pending) {
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
  task->sched_contributes_to_load = 0;
  deact(rq, task, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
  rq_unlock(rq, &rf);

  TRACE_INFO("after deactivate: on_rq=%d on_cpu=%d", task->on_rq, task->on_cpu);

  // Step 2: Set up the race scenario
  task->in_iowait = 1;
  task->on_cpu = 1; // Simulate task still in middle of schedule()

  // Step 3: Set nr_iowait to a known value
  atomic_set(&rq->nr_iowait, 5);
  int nr_iowait_before = atomic_read(&rq->nr_iowait);
  TRACE_INFO("nr_iowait before wake_up_process: %d", nr_iowait_before);

  // Step 4: Call wake_up_process from CPU 0
  // ttwu will see: on_rq=0, in_iowait=1, on_cpu=1
  // Path taken: skip ttwu_runnable → [BUGGY: dec nr_iowait] →
  //   smp_acquire → on_cpu=1 → ttwu_queue_wakelist → goto unlock
  // The task is queued in CPU 1's wakelist for deferred processing
  wake_up_process(task);

  // Step 5: Check nr_iowait IMMEDIATELY after wake_up_process returns
  // The wakelist hasn't been processed yet (no tick on CPU 1)
  int nr_iowait_after = atomic_read(&rq->nr_iowait);
  TRACE_INFO("nr_iowait after wake_up_process: %d", nr_iowait_after);

  int delta = nr_iowait_after - nr_iowait_before;
  TRACE_INFO("delta = %d", delta);

  // On BUGGY kernel:
  //   try_to_wake_up() decrements nr_iowait early, BEFORE the wakelist path.
  //   → delta = -1 (nr_iowait went from 5 to 4)
  //
  // On FIXED kernel:
  //   try_to_wake_up() does NOT decrement nr_iowait.
  //   The decrement is deferred to ttwu_do_activate() in the wakelist handler.
  //   → delta = 0 (nr_iowait stays at 5)

  if (delta == -1) {
    kstep_fail("nr_iowait decremented early in ttwu (delta=%d): "
               "race with schedule possible",
               delta);
  } else if (delta == 0) {
    kstep_pass("nr_iowait not prematurely decremented (delta=%d): "
               "decrement properly deferred to activation",
               delta);
  } else {
    kstep_fail("unexpected nr_iowait delta=%d", delta);
  }

  // Clean up: allow wakelist to be processed
  task->on_cpu = 0;
  task->in_iowait = 0;
  atomic_set(&rq->nr_iowait, 0);

  // Process pending wakeups on CPU 1
  ttwu_pending(rq);
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
