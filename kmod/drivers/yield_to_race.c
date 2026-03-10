// https://github.com/torvalds/linux/commit/5d808c78d972
//
// Bug: yield_to() uses scoped_guard(irqsave) which only disables IRQs but
// does NOT hold the target task's pi_lock. This allows try_to_wake_up() to
// concurrently migrate the target task while yield_to() is in its critical
// section, causing yield_to_task_fair()/set_next_buddy() to operate on a
// stale runqueue without proper synchronization.
//
// Fix: Change to scoped_guard(raw_spinlock_irqsave, &p->pi_lock) so that
// yield_to() holds the pi_lock, preventing ttwu from migrating the target.
//
// Detection: On the buggy kernel, a concurrent raw_spin_trylock on the
// target's pi_lock always succeeds (yield_to never holds it). On the fixed
// kernel, yield_to holds pi_lock during its critical section, so concurrent
// trylock attempts sometimes fail.

#include "internal.h"
#include <linux/kthread.h>
#include <linux/delay.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 13, 0)

static struct task_struct *target_task;
static volatile bool stop_flag;
static DECLARE_WAIT_QUEUE_HEAD(target_wq);
static volatile int target_wake;

static atomic_long_t pilock_total;
static atomic_long_t pilock_contended;
static atomic_long_t yield_calls;

static int target_fn(void *data) {
  while (!READ_ONCE(stop_flag)) {
    WRITE_ONCE(target_wake, 0);
    wait_event_interruptible(target_wq,
                             READ_ONCE(target_wake) || READ_ONCE(stop_flag));
  }
  return 0;
}

static int yielder_fn(void *data) {
  struct task_struct *p = data;
  while (!READ_ONCE(stop_flag)) {
    yield_to(p, true);
    atomic_long_inc(&yield_calls);
  }
  return 0;
}

static int checker_fn(void *data) {
  struct task_struct *p = data;
  while (!READ_ONCE(stop_flag)) {
    atomic_long_inc(&pilock_total);
    if (!raw_spin_trylock(&p->pi_lock))
      atomic_long_inc(&pilock_contended);
    else
      raw_spin_unlock(&p->pi_lock);
  }
  return 0;
}

static void setup(void) {}

static void run(void) {
  struct task_struct *yielder, *checker;

  WRITE_ONCE(stop_flag, false);
  atomic_long_set(&pilock_total, 0);
  atomic_long_set(&pilock_contended, 0);
  atomic_long_set(&yield_calls, 0);

  // Target kthread: blocks on wait queue on CPU1
  target_task = kthread_create(target_fn, NULL, "yt_target");
  if (IS_ERR(target_task)) {
    kstep_fail("failed to create target kthread");
    return;
  }
  set_cpus_allowed_ptr(target_task, cpumask_of(1));
  wake_up_process(target_task);
  kstep_tick_repeat(5);

  // Yielder kthread: calls yield_to(target) in tight loop on CPU2
  yielder = kthread_create(yielder_fn, target_task, "yt_yielder");
  if (IS_ERR(yielder)) {
    kstep_fail("failed to create yielder kthread");
    return;
  }
  set_cpus_allowed_ptr(yielder, cpumask_of(2));
  wake_up_process(yielder);

  // Checker kthread: tries pi_lock to detect if yield_to holds it, on CPU3
  checker = kthread_create(checker_fn, target_task, "yt_checker");
  if (IS_ERR(checker)) {
    kstep_fail("failed to create checker kthread");
    return;
  }
  set_cpus_allowed_ptr(checker, cpumask_of(3));
  wake_up_process(checker);

  // Let them run concurrently
  kstep_tick_repeat(30);

  // Stop all threads
  WRITE_ONCE(stop_flag, true);
  WRITE_ONCE(target_wake, 1);
  wake_up(&target_wq);
  kstep_tick_repeat(10);

  long total = atomic_long_read(&pilock_total);
  long contended = atomic_long_read(&pilock_contended);
  long ycalls = atomic_long_read(&yield_calls);

  TRACE_INFO("yield_to calls: %ld", ycalls);
  TRACE_INFO("pi_lock trylock: total=%ld contended=%ld", total, contended);

  if (total < 100) {
    kstep_fail("insufficient data: only %ld trylock attempts", total);
  } else if (contended == 0) {
    kstep_fail("yield_to never holds target pi_lock: 0/%ld contended - "
               "concurrent ttwu can migrate task during yield_to",
               total);
  } else {
    kstep_pass("yield_to holds target pi_lock: %ld/%ld contended - "
               "ttwu blocked during yield_to critical section",
               contended, total);
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "yield_to_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
