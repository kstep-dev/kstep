// https://github.com/torvalds/linux/commit/04193d590b39
// Bug: splice_balance_callbacks() removes balance_push_callback during
// __sched_setscheduler() lock break, creating a window where __schedule()
// can bypass the balance_push filter on a CPU going offline.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 19, 0)

KSYM_IMPORT(balance_push_callback);

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
}

static void run(void) {
  extern atomic_t kstep_bp_splice_count;
  struct rq *rq1 = cpu_rq(1);
  unsigned long flags;

  kstep_task_pin(task, 1, 1);
  kstep_tick_repeat(5);

  // Install balance_push_callback on CPU 1's rq (simulating CPU hotplug)
  raw_spin_lock_irqsave(&rq1->__lock, flags);
  rq1->balance_callback = KSYM_balance_push_callback;
  raw_spin_unlock_irqrestore(&rq1->__lock, flags);

  TRACE_INFO("Installed balance_push_callback on CPU 1 rq");

  // Reset the counter
  atomic_set(&kstep_bp_splice_count, 0);

  // Trigger __sched_setscheduler() from CPU 0 for the task on CPU 1.
  // sched_set_fifo() -> sched_setscheduler_nocheck() ->
  // _sched_setscheduler() -> __sched_setscheduler() which calls
  // splice_balance_callbacks(rq1).
  // In the buggy kernel, this removes balance_push_callback from the list.
  sched_set_fifo(task);

  int count = atomic_read(&kstep_bp_splice_count);
  TRACE_INFO("balance_push_callback splice count: %d", count);

  if (count > 0) {
    kstep_fail("balance_push_callback was spliced off during "
               "__sched_setscheduler (count=%d) - race window exists",
               count);
  } else {
    kstep_pass("balance_push_callback preserved during "
               "__sched_setscheduler - no race window");
  }

  // Clean up: remove balance_push_callback from rq
  raw_spin_lock_irqsave(&rq1->__lock, flags);
  if (rq1->balance_callback == KSYM_balance_push_callback)
    rq1->balance_callback = NULL;
  raw_spin_unlock_irqrestore(&rq1->__lock, flags);
}

#else
static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "balance_push_splice",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
