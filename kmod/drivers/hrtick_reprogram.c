// https://github.com/torvalds/linux/commit/156ec6f42b8d300dbbf382738ff35c8bad8f4c3a
//
// Bug: hrtimer_set_expires() in hrtick_start() modifies a queued hrtimer's
// expiry field without the hrtimer base lock, racing with
// hrtimer_start_expires() in __hrtick_restart() and corrupting the rbtree.
//
// The fix stores the expiry in rq->hrtick_time and uses hrtimer_start()
// (which properly removes and re-enqueues under the base lock) instead of
// the split hrtimer_set_expires() + hrtimer_start_expires().

#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/hrtimer.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 11, 0)

KSYM_IMPORT(hrtick_start);

static struct task_struct *tasks[4];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++) {
    tasks[i] = kstep_task_create();
    kstep_task_pin(tasks[i], 1, 1);
  }
}

// Use raw_spin_lock instead of rq_lock to avoid rq_pin_lock's reference
// to balance_push_callback (which is not exported in 5.11).
static void rq_raw_lock(struct rq *rq) {
  raw_spin_lock_irq(&rq->lock);
}

static void rq_raw_unlock(struct rq *rq) {
  raw_spin_unlock_irq(&rq->lock);
}

static void run(void) {
  struct rq *rq1 = cpu_rq(1);
  struct hrtimer *timer = &rq1->hrtick_timer;

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);
  kstep_tick_repeat(3);

  // Step 1: Queue the hrtick timer on CPU 1 by calling hrtick_start from
  // CPU 0. Since rq1 != this_rq(), this sends an async IPI to CPU 1.
  rq_raw_lock(rq1);
  KSYM_hrtick_start(rq1, 5000000000ULL); // 5s real-time delay
  rq_raw_unlock(rq1);

  // Wait for the IPI to be processed on CPU 1 and the timer to be queued.
  kstep_sleep();
  kstep_sleep();

  if (!hrtimer_active(timer)) {
    TRACE_INFO("Timer not active after first call, retrying...");
    rq_raw_lock(rq1);
    KSYM_hrtick_start(rq1, 5000000000ULL);
    rq_raw_unlock(rq1);
    kstep_sleep();
    kstep_sleep();
    kstep_sleep();
  }

  if (!hrtimer_active(timer)) {
    kstep_pass("hrtick timer not active - cannot test");
    return;
  }

  ktime_t initial_expiry = hrtimer_get_expires(timer);
  TRACE_INFO("hrtick timer queued, expiry=%lld", initial_expiry);

  // Step 2: Hold rq lock and call hrtick_start again with a different delay.
  // Holding the lock prevents the new IPI from being processed on CPU 1,
  // so we can observe the timer state immediately after hrtick_start returns.
  //
  // Buggy kernel: hrtimer_set_expires() directly modifies the queued timer's
  //   expiry field in the rbtree, corrupting the tree structure.
  // Fixed kernel: only rq->hrtick_time is updated; the timer is untouched.
  rq_raw_lock(rq1);

  ktime_t expiry_before = hrtimer_get_expires(timer);
  KSYM_hrtick_start(rq1, 9000000000ULL); // 9s (different from 5s)
  ktime_t expiry_after = hrtimer_get_expires(timer);
  bool still_active = hrtimer_active(timer);

  rq_raw_unlock(rq1);

  s64 delta = (s64)(expiry_after - expiry_before);
  TRACE_INFO("before=%lld, after=%lld, delta=%lld, active=%d",
             expiry_before, expiry_after, delta, still_active);

  if (still_active && delta != 0) {
    kstep_fail("hrtimer_set_expires modified queued hrtick timer: "
               "expiry changed by %lld ns while in rbtree", delta);
  } else {
    kstep_pass("hrtick_start did not modify queued timer expiry (fixed)");
  }
}

#else

static void setup(void) {}
static void run(void) {
  kstep_pass("Skipped: kernel version mismatch");
}

#endif

KSTEP_DRIVER_DEFINE{
    .name = "hrtick_reprogram",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 10000,
};
