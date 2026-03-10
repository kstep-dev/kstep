// https://github.com/torvalds/linux/commit/15257cc2f905
//
// Bug: check_preempt_wakeup_fair() forces preemption when the waker becomes
// ineligible (vruntime > avg), bypassing run-to-parity and slice protection.
// This causes excessive rescheduling without guaranteeing the wakee runs next.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 19, 0)

static struct task_struct *waker;     // Task A
static struct task_struct *competitor; // Task B
static struct task_struct *wakee;     // Task C

static void setup(void) {
  waker = kstep_kthread_create("waker_A");
  competitor = kstep_kthread_create("comp_B");
  wakee = kstep_kthread_create("wakee_C");

  kstep_kthread_bind(waker, cpumask_of(1));
  kstep_kthread_bind(competitor, cpumask_of(1));
  kstep_kthread_bind(wakee, cpumask_of(1));
}

static void run(void) {
  struct rq *rq = cpu_rq(1);

  // Start wakee first, let it run, then block it
  kstep_kthread_start(wakee);
  kstep_sleep();
  TRACE_INFO("wakee started: CPU1 curr=%s", rq->curr->comm);

  kstep_kthread_block(wakee);
  kstep_sleep();
  TRACE_INFO("wakee blocked: CPU1 curr=%s", rq->curr->comm);

  // Now start waker - it becomes current on CPU 1
  kstep_kthread_start(waker);
  kstep_sleep();
  TRACE_INFO("waker started: CPU1 curr=%s", rq->curr->comm);

  // Start competitor - goes to runqueue but can't preempt waker (PREEMPT_NONE)
  kstep_kthread_start(competitor);
  kstep_sleep();
  TRACE_INFO("competitor started: CPU1 curr=%s, waker eligible=%d",
             rq->curr->comm, kstep_eligible(&waker->se));

  // Tick to advance waker's vruntime until it becomes ineligible
  int tick_count = 0;
  while (rq->curr != waker || kstep_eligible(&waker->se)) {
    TRACE_INFO("tick %d: curr=%s eligible=%d",
               tick_count, rq->curr->comm,
               (rq->curr == waker) ? kstep_eligible(&waker->se) : -1);
    if (tick_count > 50) {
      kstep_fail("timed out waiting for waker to be current and ineligible");
      return;
    }
    kstep_tick();
    tick_count++;
  }

  TRACE_INFO("waker is current and ineligible after %d ticks", tick_count);

  // Clear NEED_RESCHED flags to isolate the wakeup effect
  clear_tsk_need_resched(waker);

  // A wakes C but continues spinning
  kstep_kthread_wake_continue(waker, wakee);
  kstep_sleep();

  // Check if NEED_RESCHED was set by the wakeup path
  // Buggy: forced preemption sets resched on ineligible waker
  // Fixed: no forced preemption, flag stays clear
  int need_resched = test_tsk_need_resched(waker);
  TRACE_INFO("After wakeup: curr=%s need_resched=%d",
             rq->curr->comm, need_resched);

  if (!need_resched) {
    kstep_pass("no forced preemption on wakeup");
  } else {
    kstep_fail("forced preemption set on ineligible waker (bug)");
  }

  kstep_tick_repeat(5);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "revert_force_wakeup_preemption",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
