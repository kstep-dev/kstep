#include "driver.h"
#include "internal.h"
#include <linux/delay.h>
#include <linux/kprobes.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

#define TARGET_CPU 1
#define NUM_TASKS 3

KSYM_IMPORT(sysctl_sched_features);

static struct task_struct *tasks[NUM_TASKS];
static atomic_t hrtick_start_count = ATOMIC_INIT(0);
static bool counting_enabled;

static int hrtick_start_probe(struct kprobe *p, struct pt_regs *regs) {
  if (counting_enabled && smp_processor_id() == TARGET_CPU)
    atomic_inc(&hrtick_start_count);
  return 0;
}

static struct kprobe hrtick_start_kp = {
    .symbol_name = "hrtick_start",
    .pre_handler = hrtick_start_probe,
};

/* Simulate hrtick callback. Clear TIF_NEED_RESCHED before task_tick to
 * simulate the PREEMPT_LAZY condition where resched_curr_lazy only sets
 * the lazy flag. entity_tick(queued=1) calls resched_curr_lazy which
 * in PREEMPT_LAZY mode sets TIF_NEED_RESCHED_LAZY, leaving need_resched()
 * false. We emulate this by clearing TIF_NEED_RESCHED after entity_tick
 * sets it (since our kernel config has PREEMPT_NONE where lazy=full). */
static void simulate_hrtick_preempt_lazy(void *info) {
  struct rq *rq = this_rq();
  struct task_struct *donor = rq->donor;
  unsigned long flags;

  if (!donor || !donor->sched_class)
    return;

  raw_spin_lock_irqsave(&rq->__lock, flags);
  /* Clear any stale resched flags */
  clear_tsk_need_resched(rq->curr);
  /* Call task_tick(queued=1). entity_tick will set TIF_NEED_RESCHED
   * via resched_curr_lazy (since our config maps lazy→full).
   * We clear it after to emulate PREEMPT_LAZY behavior. */
  donor->sched_class->task_tick(rq, donor, 1);
  /* Undo the TIF_NEED_RESCHED that entity_tick set, simulating
   * PREEMPT_LAZY where only TIF_NEED_RESCHED_LAZY would be set.
   * Then re-enter the queued path logic manually. */
  clear_tsk_need_resched(rq->curr);
  raw_spin_unlock_irqrestore(&rq->__lock, flags);
}

/* Check if hrtick_start_fair would call hrtick_start in PREEMPT_LAZY
 * conditions by calling it directly (bypasses the task_tick_fair guard) */
typedef void(hrtick_start_fair_t)(struct rq *, struct task_struct *);
KSYM_IMPORT_TYPED(hrtick_start_fair_t, hrtick_start_fair);

static void call_hrtick_start_fair(void *info) {
  struct rq *rq = this_rq();
  unsigned long flags;

  raw_spin_lock_irqsave(&rq->__lock, flags);
  if (rq->donor)
    KSYM_hrtick_start_fair(rq, rq->donor);
  raw_spin_unlock_irqrestore(&rq->__lock, flags);
}

static void setup(void) {
  for (int i = 0; i < NUM_TASKS; i++) {
    tasks[i] = kstep_task_create();
    kstep_task_pin(tasks[i], TARGET_CPU, TARGET_CPU);
  }
  if (register_kprobe(&hrtick_start_kp) < 0)
    panic("kprobe on hrtick_start failed");
}

static void run(void) {
  struct rq *rq = cpu_rq(TARGET_CPU);

  *KSYM_sysctl_sched_features |= (1UL << __SCHED_FEAT_HRTICK);
  for (int i = 0; i < NUM_TASKS; i++)
    kstep_task_wakeup(tasks[i]);
  kstep_tick_repeat(5);

  /* Test 1: With HRTICK on, direct call should arm timer */
  counting_enabled = true;
  atomic_set(&hrtick_start_count, 0);
  smp_call_function_single(TARGET_CPU, call_hrtick_start_fair, NULL, 1);
  int direct_on = atomic_read(&hrtick_start_count);
  TRACE_INFO("HRTICK on, direct call: starts=%d", direct_on);

  /* Disable HRTICK */
  *KSYM_sysctl_sched_features &= ~(1UL << __SCHED_FEAT_HRTICK);

  /* Test 2: With HRTICK off, direct call still arms (no guard in fn) */
  atomic_set(&hrtick_start_count, 0);
  smp_call_function_single(TARGET_CPU, call_hrtick_start_fair, NULL, 1);
  int direct_off = atomic_read(&hrtick_start_count);
  TRACE_INFO("HRTICK off, direct call: starts=%d", direct_off);

  /* Test 3: Simulate hrtick callback with PREEMPT_LAZY emulation.
   * On buggy kernel: task_tick_fair(q=1) reaches hrtick_start_fair
   *   without hrtick_enabled_fair check → hrtick_start called.
   * On fixed kernel: hrtick_enabled_fair check prevents call. */
  atomic_set(&hrtick_start_count, 0);
  smp_call_function_single(TARGET_CPU, simulate_hrtick_preempt_lazy, NULL, 1);
  int sim_off = atomic_read(&hrtick_start_count);
  TRACE_INFO("HRTICK off, simulated hrtick (PREEMPT_LAZY): starts=%d", sim_off);

  counting_enabled = false;
  hrtimer_cancel(&rq->hrtick_timer);

  if (direct_on == 0) {
    kstep_fail("kprobe not working (direct_on=0)");
  } else if (direct_off > 0 || sim_off > 0) {
    kstep_fail("hrtick_start called with HRTICK disabled: "
               "direct=%d simulated=%d (on=%d). "
               "Missing hrtick_enabled_fair check in "
               "task_tick_fair queued path.",
               direct_off, sim_off, direct_on);
  } else {
    kstep_pass("hrtick properly guarded (on=%d direct_off=%d "
               "sim_off=%d)",
               direct_on, direct_off, sim_off);
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
