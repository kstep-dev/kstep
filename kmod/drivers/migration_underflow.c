// https://github.com/torvalds/linux/commit/9d0df37797453f168afdb2e6fd0353c73718ae9a
//
// Bug: migrate_enable() without matching migrate_disable() silently
// underflows rq->nr_pinned counter. The fix adds WARN_ON_ONCE check.

#include "driver.h"
#include "internal.h"
#include <linux/preempt.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
    LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)

static void setup(void) {}

static void run(void) {
  struct rq *rq;
  unsigned int nr_pinned_before, nr_pinned_after;

  // current->migration_disabled should be 0 initially
  TRACE_INFO("migration_disabled=%d", current->migration_disabled);

  if (current->migration_disabled != 0) {
    kstep_fail("migration_disabled already non-zero: %d",
               current->migration_disabled);
    return;
  }

  preempt_disable();
  rq = this_rq();
  nr_pinned_before = rq->nr_pinned;
  preempt_enable();

  TRACE_INFO("BEFORE: nr_pinned=%u", nr_pinned_before);

  // Call migrate_enable() without matching migrate_disable().
  // Buggy kernel: nr_pinned underflows (unsigned 0 -> UINT_MAX).
  // Fixed kernel: WARN_ON_ONCE fires and returns early.
  migrate_enable();

  preempt_disable();
  rq = this_rq();
  nr_pinned_after = rq->nr_pinned;
  preempt_enable();

  TRACE_INFO("AFTER: nr_pinned=%u migration_disabled=%d",
             nr_pinned_after, current->migration_disabled);

  if (nr_pinned_after != nr_pinned_before) {
    kstep_fail("nr_pinned changed: %u -> %u (underflow bug)",
               nr_pinned_before, nr_pinned_after);
  } else {
    kstep_pass("nr_pinned unchanged: %u (no underflow)",
               nr_pinned_before);
  }
}

#else

static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}

#endif

KSTEP_DRIVER_DEFINE{
    .name = "migration_underflow",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
