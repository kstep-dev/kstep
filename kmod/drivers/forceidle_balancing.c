// https://github.com/torvalds/linux/commit/5b6547ed97f4
//
// Bug: queue_core_balance() is called from set_next_task_idle(), which fires
// every time the idle task is selected as "next" — including from non-schedule
// contexts (rt_mutex_setprio's "change" pattern). This can cause the forceidle
// balancer to be queued outside the __schedule() rq->lock context.
//
// Fix: Move queue_core_balance() from set_next_task_idle() into
// pick_next_task(), guarded by (core_forceidle_count && next == rq->idle).
//
// Test: Directly enable core scheduling state on rq1, set up conditions for
// queue_core_balance to fire (core_cookie != 0, nr_running > 0), then invoke
// set_next_task_idle() via the idle sched class. In the buggy kernel, the
// balance callback is queued inside set_next_task_idle. In the fixed kernel,
// set_next_task_idle does NOT queue the callback (it was moved to
// pick_next_task).

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 18, 0)

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
}

static void run(void) {
  struct rq *rq1 = cpu_rq(1);
  unsigned long flags;

  // Pin a task to CPU 1 and let it run
  kstep_task_pin(task, 1, 1);
  kstep_tick_repeat(3);

  TRACE_INFO("CPU1 nr_running=%d, curr=%s (pid=%d)",
             rq1->nr_running, rq1->curr->comm, rq1->curr->pid);

  // Directly enable core scheduling state without calling sched_core_get()
  // (which calls synchronize_rcu and hangs in kSTEP).
  // 1. Enable the __sched_core_enabled static branch
  KSYM_IMPORT(__sched_core_enabled);
  static_branch_enable(KSYM___sched_core_enabled);

  // 2. Set core_enabled on rq1
  raw_spin_rq_lock_irqsave(rq1, flags);

  bool orig_core_enabled = rq1->core_enabled;
  unsigned long orig_core_cookie = rq1->core->core_cookie;
  struct callback_head *orig_balance_cb = rq1->balance_callback;

  rq1->core_enabled = true;

  // Verify core scheduling is now enabled on rq1
  if (!sched_core_enabled(rq1)) {
    raw_spin_rq_unlock_irqrestore(rq1, flags);
    kstep_fail("sched_core_enabled is false after manual enable");
    return;
  }
  TRACE_INFO("Core scheduling manually enabled on CPU 1");

  // Set up conditions for queue_core_balance to fire:
  // - sched_core_enabled(rq) = true (done above)
  // - rq->core->core_cookie != 0
  // - rq->nr_running != 0 (task is pinned and running on CPU 1)
  rq1->core->core_cookie = 1;
  TRACE_INFO("CPU1: nr_running=%d, core_cookie=%lu, core_enabled=%d",
             rq1->nr_running, rq1->core->core_cookie, rq1->core_enabled);

  // Clear balance_callback before test
  rq1->balance_callback = NULL;

  // Invoke set_next_task_idle() via the idle sched class.
  // In the BUGGY kernel: set_next_task_idle → queue_core_balance → callback queued
  // In the FIXED kernel: set_next_task_idle does NOT call queue_core_balance
  rq1->idle->sched_class->set_next_task(rq1, rq1->idle, false);

  // Check if a balance callback was queued
  int cb_queued = (rq1->balance_callback != NULL);
  TRACE_INFO("After set_next_task_idle: balance_callback=%p (queued=%d)",
             rq1->balance_callback, cb_queued);

  // Clean up: restore original state
  rq1->balance_callback = orig_balance_cb;
  rq1->core->core_cookie = orig_core_cookie;
  rq1->core_enabled = orig_core_enabled;

  raw_spin_rq_unlock_irqrestore(rq1, flags);

  // Disable core scheduling static branch
  static_branch_disable(KSYM___sched_core_enabled);

  kstep_tick_repeat(3);

  if (cb_queued) {
    kstep_fail("queue_core_balance called from set_next_task_idle — "
               "forceidle balancer queued from wrong context (buggy)");
  } else {
    kstep_pass("set_next_task_idle does NOT call queue_core_balance — "
               "forceidle balancer only queued from pick_next_task (fixed)");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "forceidle_balancing",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
