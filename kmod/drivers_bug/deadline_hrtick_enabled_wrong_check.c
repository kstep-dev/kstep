#include "driver.h"
#include "internal.h"
#include <linux/kprobes.h>
#include <linux/version.h>

/*
 * Bug: commit 95a0155224a6 ("sched/fair: Limit hrtick work") added
 * a call to hrtick_start_fair() in task_tick_fair()'s queued path
 * without a hrtick_enabled_fair() guard:
 *
 *   if (queued) {
 *       if (!need_resched())
 *           hrtick_start_fair(rq, curr);  // no hrtick_enabled_fair check!
 *       return;
 *   }
 *
 * With CONFIG_PREEMPT_LAZY, entity_tick(queued=1) calls
 * resched_curr_lazy() which only sets TIF_NEED_RESCHED_LAZY, so
 * need_resched() returns false. This means every hrtick callback
 * re-arms the timer through hrtick_start_fair() even after HRTICK
 * is disabled, creating an endless timer loop.
 *
 * Fix: add hrtick_enabled_fair() check inside hrtick_start_fair().
 *
 * Test: enable HRTICK, create competing CFS tasks, disable HRTICK,
 * then simulate a hrtick callback via task_tick(queued=1). Count
 * hrtick_start() calls — should be zero when HRTICK is disabled.
 */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)

#define TARGET_CPU 1
#define NUM_TASKS 3

KSYM_IMPORT(sysctl_sched_features);

static struct task_struct *tasks[NUM_TASKS];
static atomic_t hrtick_start_count = ATOMIC_INIT(0);
static bool counting_enabled;

static int hrtick_start_probe(struct kprobe *p, struct pt_regs *regs)
{
  if (counting_enabled && smp_processor_id() == TARGET_CPU)
    atomic_inc(&hrtick_start_count);
  return 0;
}

static struct kprobe hrtick_start_kp = {
    .symbol_name = "hrtick_start",
    .pre_handler = hrtick_start_probe,
};

/*
 * Simulate the hrtick timer callback (kernel/sched/core.c hrtick()):
 *   task_tick(rq, rq->donor, queued=1)
 *
 * Clear TIF_NEED_RESCHED beforehand so that the queued path in
 * task_tick_fair reaches hrtick_start_fair (need_resched() == false).
 * With PREEMPT_LAZY, entity_tick(queued=1) only sets the lazy flag,
 * so need_resched() stays false naturally; the clear is a safeguard
 * in case it was set by an unrelated path.
 */
static void simulate_hrtick_callback(void *info)
{
  struct rq *rq = this_rq();
  unsigned long flags;

  raw_spin_lock_irqsave(&rq->__lock, flags);
  if (rq->donor && rq->donor->sched_class) {
    clear_tsk_need_resched(rq->curr);
    rq->donor->sched_class->task_tick(rq, rq->donor, 1);
  }
  raw_spin_unlock_irqrestore(&rq->__lock, flags);
}

static void setup(void)
{
  for (int i = 0; i < NUM_TASKS; i++) {
    tasks[i] = kstep_task_create();
    kstep_task_pin(tasks[i], TARGET_CPU, TARGET_CPU);
  }
  if (register_kprobe(&hrtick_start_kp) < 0)
    panic("kprobe on hrtick_start failed");
}

static void run(void)
{
  struct rq *rq = cpu_rq(TARGET_CPU);

  /* Enable HRTICK and let tasks run to establish baseline */
  *KSYM_sysctl_sched_features |= (1UL << __SCHED_FEAT_HRTICK);
  for (int i = 0; i < NUM_TASKS; i++)
    kstep_task_wakeup(tasks[i]);
  kstep_tick_repeat(5);

  /* Baseline: simulate hrtick callback with HRTICK on */
  counting_enabled = true;
  atomic_set(&hrtick_start_count, 0);
  smp_call_function_single(TARGET_CPU, simulate_hrtick_callback, NULL, 1);
  int on_count = atomic_read(&hrtick_start_count);
  TRACE_INFO("HRTICK enabled: hrtick_start calls=%d (expect >0)", on_count);

  /* Disable HRTICK and cancel any armed timer */
  *KSYM_sysctl_sched_features &= ~(1UL << __SCHED_FEAT_HRTICK);
  hrtimer_cancel(&rq->hrtick_timer);
  kstep_tick_repeat(2);

  /*
   * Simulate a hrtick timer firing AFTER HRTICK was disabled.
   * Buggy:  task_tick_fair(queued=1) → hrtick_start_fair → hrtick_start
   * Fixed:  hrtick_start_fair checks hrtick_enabled_fair → returns early
   */
  atomic_set(&hrtick_start_count, 0);
  smp_call_function_single(TARGET_CPU, simulate_hrtick_callback, NULL, 1);
  int off_count = atomic_read(&hrtick_start_count);
  TRACE_INFO("HRTICK disabled: hrtick_start calls=%d (expect 0)", off_count);

  counting_enabled = false;
  hrtimer_cancel(&rq->hrtick_timer);

  if (on_count == 0) {
    kstep_fail("kprobe not working: hrtick_start not called "
               "with HRTICK enabled");
  } else if (off_count > 0) {
    kstep_fail("hrtick_start called %d time(s) with HRTICK "
               "disabled (on=%d). Missing hrtick_enabled_fair "
               "guard in task_tick_fair queued path.",
               off_count, on_count);
  } else {
    kstep_pass("hrtick properly guarded: on=%d off=%d",
               on_count, off_count);
  }

  *KSYM_sysctl_sched_features &= ~(1UL << __SCHED_FEAT_HRTICK);
  unregister_kprobe(&hrtick_start_kp);
  kstep_tick_repeat(3);
}

KSTEP_DRIVER_DEFINE{
    .name = "deadline_hrtick_enabled_wrong_check",
    .setup = setup,
    .run = run,
    .step_interval_us = 2000,
};
#endif
