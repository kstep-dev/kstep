// Reproduce: rq-qos missed wake-ups (commit 11c7aa0ddea8)
//
// Bug: prepare_to_wait_exclusive() returns void, forcing rq_qos_wait() to use
// the racy wq_has_single_sleeper() check after adding to the queue. When two
// waiters add simultaneously, both see has_sleeper=true and skip the acquire
// recheck, causing a deadlock.
//
// Fix: prepare_to_wait_exclusive() returns bool (was queue empty before add?),
// so the first waiter atomically knows it was first and rechecks.

#include "driver.h"
#include "internal.h"
#include <linux/version.h>
#include <linux/wait.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 13, 0)

// Detect at compile time whether prepare_to_wait_exclusive returns bool
typedef bool (*ptwe_bool_fn_t)(struct wait_queue_head *,
                               struct wait_queue_entry *, int);
#define PTWE_RETURNS_BOOL \
  __builtin_types_compatible_p(typeof(&prepare_to_wait_exclusive), \
                               ptwe_bool_fn_t)

static DECLARE_WAIT_QUEUE_HEAD(test_wq);
static struct wait_queue_entry wq1, wq2;

static void setup(void) {
  init_waitqueue_head(&test_wq);
  init_wait_entry(&wq1, 0);
  init_wait_entry(&wq2, 0);
}

static void run(void) {
  if (PTWE_RETURNS_BOOL) {
    // Fixed kernel: prepare_to_wait_exclusive returns bool.
    // Look up via ksym to avoid compile-time type conflict.
    ptwe_bool_fn_t fn =
        (ptwe_bool_fn_t)kstep_ksym_lookup("prepare_to_wait_exclusive");

    // Add waiter1: queue is empty → returns true
    bool was_empty1 = fn(&test_wq, &wq1, TASK_RUNNING);
    // Add waiter2: queue has waiter1 → returns false
    bool was_empty2 = fn(&test_wq, &wq2, TASK_RUNNING);

    bool has_sleeper1 = !was_empty1;
    bool has_sleeper2 = !was_empty2;

    TRACE_INFO("Fixed: w1 was_empty=%d has_sleeper=%d, "
               "w2 was_empty=%d has_sleeper=%d",
               was_empty1, has_sleeper1, was_empty2, has_sleeper2);

    // First waiter got was_empty=true → has_sleeper=false → would recheck
    if (!has_sleeper1)
      kstep_pass("First waiter rechecks acquire - no deadlock");
    else
      kstep_fail("First waiter skips recheck on fixed kernel");
  } else {
    // Buggy kernel: prepare_to_wait_exclusive returns void.
    // Both waiters add themselves to the queue.
    prepare_to_wait_exclusive(&test_wq, &wq1, TASK_RUNNING);
    prepare_to_wait_exclusive(&test_wq, &wq2, TASK_RUNNING);

    // Racy check: after both are added, wq_has_single_sleeper is false
    // for both → has_sleeper=true for both → both skip acquire recheck
    bool has_sleeper1 = !wq_has_single_sleeper(&test_wq);
    bool has_sleeper2 = !wq_has_single_sleeper(&test_wq);

    TRACE_INFO("Buggy: has_sleeper1=%d has_sleeper2=%d",
               has_sleeper1, has_sleeper2);

    if (has_sleeper1 && has_sleeper2)
      kstep_fail("Both waiters skip acquire recheck - deadlock");
    else
      kstep_pass("At least one waiter rechecks (unexpected)");
  }

  finish_wait(&test_wq, &wq1);
  finish_wait(&test_wq, &wq2);
}

KSTEP_DRIVER_DEFINE{
    .name = "rq_qos_wakeup",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#endif
