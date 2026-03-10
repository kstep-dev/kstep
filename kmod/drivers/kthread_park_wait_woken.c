// https://github.com/torvalds/linux/commit/ef73d6a4ef0b
//
// Bug: wait_woken() only checks kthread_should_stop() but not
// kthread_should_park(). When kthread_park() is called on a kthread
// that's in a wait_woken() loop, the kthread re-enters schedule_timeout()
// instead of waking up to handle the park request.
//
// Reproduce: Create a kthread using the wait_woken() pattern with a
// condition that never becomes true. Set KTHREAD_SHOULD_PARK on it and
// wake it. On buggy kernel, the kthread re-enters schedule_timeout()
// (ignoring SHOULD_PARK). On fixed kernel, wait_woken() returns
// immediately because kthread_should_stop_or_park() is true.

#include "internal.h"
#include <linux/kthread.h>
#include <linux/wait.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 4, 0)

// Mirror kernel's internal kthread struct (only need flags at offset 0)
struct kthread_mirror {
  unsigned long flags;
};
#define KTHREAD_SHOULD_PARK_BIT 2

static DECLARE_WAIT_QUEUE_HEAD(test_wq);
static struct task_struct *victim;
static atomic_t woken_count = ATOMIC_INIT(0);

// Kthread that uses wait_woken() in a loop with a condition that never
// becomes true. This pattern exposes the bug: after being woken by
// wake_up_process() (from kthread_park), the kthread loops back into
// wait_woken() which doesn't check KTHREAD_SHOULD_PARK on buggy kernels.
static int victim_fn(void *data) {
  DEFINE_WAIT_FUNC(wait, woken_wake_function);

  add_wait_queue(&test_wq, &wait);

  while (!kthread_should_stop()) {
    wait_woken(&wait, TASK_INTERRUPTIBLE, MAX_SCHEDULE_TIMEOUT);
    atomic_inc(&woken_count);
  }

  remove_wait_queue(&test_wq, &wait);
  return 0;
}

static void setup(void) {
  victim = kthread_create(victim_fn, NULL, "victim_kt");
  if (IS_ERR(victim)) {
    TRACE_INFO("failed to create victim kthread");
    return;
  }
  kthread_bind(victim, 1);
}

static void *victim_sleeping(void) {
  return (READ_ONCE(victim->__state) == TASK_INTERRUPTIBLE) ? (void *)1 : NULL;
}

static void run(void) {
  struct kthread_mirror *kt;
  int count_before, count_after;

  wake_up_process(victim);

  // Wait until victim enters TASK_INTERRUPTIBLE (sleeping in schedule_timeout)
  kstep_sleep_until(victim_sleeping);
  kstep_tick_repeat(3);

  if (READ_ONCE(victim->__state) != TASK_INTERRUPTIBLE) {
    kstep_fail("victim did not enter TASK_INTERRUPTIBLE");
    kthread_stop(victim);
    return;
  }

  TRACE_INFO("victim sleeping in wait_woken (pid=%d, state=%u)",
             victim->pid, READ_ONCE(victim->__state));

  // Directly set KTHREAD_SHOULD_PARK bit (mirrors what kthread_park does
  // internally, without the wait_for_completion that would block us)
  kt = (struct kthread_mirror *)victim->worker_private;
  if (!kt) {
    kstep_fail("victim worker_private is NULL");
    kthread_stop(victim);
    return;
  }
  set_bit(KTHREAD_SHOULD_PARK_BIT, &kt->flags);
  smp_mb();

  // Wake victim from first schedule_timeout sleep
  wake_up_process(victim);

  count_before = atomic_read(&woken_count);
  TRACE_INFO("woken_count before ticks: %d", count_before);

  // Tick to let victim run. On fixed kernel, victim busy-loops in
  // wait_woken (returns immediately each time). On buggy kernel,
  // victim re-enters schedule_timeout and sleeps forever.
  kstep_tick_repeat(20);

  count_after = atomic_read(&woken_count);
  TRACE_INFO("woken_count after ticks: %d, victim state: %u",
             count_after, READ_ONCE(victim->__state));

  if (count_after <= count_before + 1) {
    kstep_fail("wait_woken ignores KTHREAD_SHOULD_PARK "
               "(woken %d->%d, state=%u)",
               count_before, count_after, READ_ONCE(victim->__state));
  } else {
    kstep_pass("wait_woken handles KTHREAD_SHOULD_PARK "
               "(woken %d->%d)",
               count_before, count_after);
  }

  // Cleanup: clear SHOULD_PARK and stop the kthread
  clear_bit(KTHREAD_SHOULD_PARK_BIT, &kt->flags);
  kthread_stop(victim);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "kthread_park_wait_woken",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
