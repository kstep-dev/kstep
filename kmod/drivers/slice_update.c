// https://github.com/torvalds/linux/commit/2f2fc17bab0011430ceb6f2dc1959e7d1f981444
//
// Bug: place_entity() does not update se->slice from sysctl_sched_base_slice.
// Tasks that sleep and wake up retain their stale slice from fork or the last
// update_deadline() call, causing incorrect deadline computation.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
}

static void run(void) {
  KSYM_IMPORT(sysctl_sched_base_slice);
  unsigned int original_slice = *KSYM_sysctl_sched_base_slice;

  TRACE_INFO("original sysctl_sched_base_slice = %u", original_slice);
  TRACE_INFO("fork-time slice = %llu", task->se.slice);

  // Change sysctl BEFORE waking the task. The task was created with
  // se->slice = original_slice (set in __sched_fork). It has never run,
  // so update_deadline() has never fired.
  unsigned int new_slice = original_slice * 10;
  *KSYM_sysctl_sched_base_slice = new_slice;
  TRACE_INFO("changed sysctl_sched_base_slice to %u", new_slice);

  // Wake the task. try_to_wake_up() -> enqueue_entity() -> place_entity()
  // is called synchronously. On the fixed kernel, place_entity() updates
  // se->slice = sysctl_sched_base_slice. On the buggy kernel, it doesn't.
  kstep_task_wakeup(task);

  // Read slice immediately (no ticks have occurred, so update_deadline()
  // has not had a chance to update the slice).
  u64 slice = task->se.slice;
  TRACE_INFO("slice after wakeup = %llu", slice);

  if (slice == (u64)original_slice) {
    kstep_fail("stale slice: got=%llu expected=%u", slice, new_slice);
  } else {
    kstep_pass("slice updated: got=%llu expected=%u", slice, new_slice);
  }
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "slice_update",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
