// Reproducer for: sched/core: Add WARN_ON_ONCE() to check overflow for migrate_disable()
// Commit: 0ec8d5aed4d1
//
// The bug: migration_disabled (unsigned short) can overflow to 0 after 65536
// migrate_disable() calls without matching migrate_enable(). After overflow,
// calling migrate_enable() behaves differently:
//   Buggy kernel: unconditional WARN_ON_ONCE(!migration_disabled) fires, returns
//                 early -> nr_pinned NOT decremented (leak)
//   Fixed kernel: that WARN was moved under CONFIG_DEBUG_PREEMPT, so the full
//                 migration path runs -> nr_pinned properly decremented

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 11, 0)

static void setup(void) {}

static void run(void) {
  struct task_struct *p = current;
  int nr_pinned_before, nr_pinned_after;
  int i;

  preempt_disable();
  nr_pinned_before = this_rq()->nr_pinned;
  preempt_enable();

  TRACE_INFO("initial: migration_disabled=%u nr_pinned=%d",
             p->migration_disabled, nr_pinned_before);

  // Call migrate_disable() 65536 times to overflow the u16 counter.
  // 1st call: sets migration_disabled=1, increments nr_pinned.
  // Calls 2..65535: just increment migration_disabled.
  // Call 65536: migration_disabled wraps from 65535 to 0.
  for (i = 0; i < 65536; i++)
    migrate_disable();

  TRACE_INFO("after 65536 migrate_disable(): migration_disabled=%u",
             p->migration_disabled);

  // Call migrate_enable() once.
  // Buggy: WARN_ON_ONCE(!migration_disabled) -> true -> return early (nr_pinned leak)
  // Fixed: no unconditional WARN, full path runs -> nr_pinned decremented
  migrate_enable();

  preempt_disable();
  nr_pinned_after = this_rq()->nr_pinned;
  preempt_enable();

  TRACE_INFO("after migrate_enable(): migration_disabled=%u nr_pinned=%d",
             p->migration_disabled, nr_pinned_after);

  if (nr_pinned_after > nr_pinned_before) {
    kstep_fail("nr_pinned leaked (%d -> %d)", nr_pinned_before, nr_pinned_after);
    // Clean up the leak so the framework state remains consistent
    preempt_disable();
    this_rq()->nr_pinned--;
    preempt_enable();
  } else {
    kstep_pass("nr_pinned correct (%d -> %d)", nr_pinned_before, nr_pinned_after);
  }
}

#else
static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "migrate_overflow",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
