// https://github.com/torvalds/linux/commit/abfc01077df6
//
// Bug: do_set_cpus_allowed() calls __do_set_cpus_allowed() without holding
// rq->lock and without calling update_rq_clock(), violating the locking
// contract that __do_set_cpus_allowed() requires both p->pi_lock and rq->lock.
//
// Fix: Wraps with scoped_guard(__task_rq_lock, p) to properly hold rq->lock
// and update the rq clock before dequeue/enqueue operations.
//
// Detection: Advance the virtual sched_clock without a full tick, creating a
// gap between rq->clock and sched_clock. Then call do_set_cpus_allowed on a
// queued task. On the fixed kernel, update_rq_clock() inside
// do_set_cpus_allowed brings rq->clock up to date. On the buggy kernel,
// rq->clock stays stale because there is no update_rq_clock call.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

static struct task_struct *task;

static void setup(void) { task = kstep_task_create(); }

static void run(void) {
  typedef void (*do_set_cpus_allowed_fn_t)(struct task_struct *,
                                           const struct cpumask *);

  do_set_cpus_allowed_fn_t do_set_fn =
      (do_set_cpus_allowed_fn_t)kstep_ksym_lookup("do_set_cpus_allowed");
  if (!do_set_fn) {
    kstep_fail("cannot resolve do_set_cpus_allowed");
    return;
  }

  kstep_task_pin(task, 1, 1);
  kstep_tick_repeat(5);

  struct rq *rq1 = cpu_rq(1);
  bool queued = task_on_rq_queued(task);
  TRACE_INFO("task pid=%d on_rq=%d queued=%d cpu=%d", task->pid, task->on_rq,
             queued, task_cpu(task));

  if (!queued) {
    kstep_fail("task not queued on CPU 1");
    return;
  }

  // Advance sched_clock WITHOUT a full scheduler tick. This creates a gap
  // between the stale rq->clock (last updated during the previous tick) and
  // the now-advanced sched_clock.
  kstep_sched_clock_tick();

  u64 clock_before = rq1->clock;
  u64 sched_now = kstep_sched_clock_get();
  TRACE_INFO("clock gap: rq->clock=%llu sched_clock=%llu delta=%llu",
             clock_before, sched_now, sched_now - clock_before);

  // Call do_set_cpus_allowed with pi_lock held (as all callers do).
  // Buggy kernel: calls __do_set_cpus_allowed directly, no rq->lock,
  //   no update_rq_clock → rq->clock stays stale.
  // Fixed kernel: acquires rq->lock via scoped_guard(__task_rq_lock),
  //   calls update_rq_clock → rq->clock advances.
  unsigned long flags;
  raw_spin_lock_irqsave(&task->pi_lock, flags);
  do_set_fn(task, cpu_possible_mask);
  raw_spin_unlock_irqrestore(&task->pi_lock, flags);

  u64 clock_after = rq1->clock;
  TRACE_INFO("after: rq->clock=%llu (was %llu, delta=%llu)", clock_after,
             clock_before, clock_after - clock_before);

  if (clock_after == clock_before) {
    kstep_fail("rq->clock not updated during do_set_cpus_allowed: "
               "missing rq->lock and update_rq_clock (locking bug)");
  } else {
    kstep_pass("rq->clock updated: do_set_cpus_allowed properly holds "
               "rq->lock and calls update_rq_clock");
  }

  kstep_tick_repeat(3);
}

KSTEP_DRIVER_DEFINE{
    .name = "set_cpus_allowed_lock",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};

#else
static void run(void) { TRACE_INFO("Skipped: wrong kernel version"); }
KSTEP_DRIVER_DEFINE{.name = "set_cpus_allowed_lock", .run = run};
#endif
