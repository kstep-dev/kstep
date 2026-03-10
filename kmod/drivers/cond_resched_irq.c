// Bug: cond_resched() calls schedule() even when interrupts are disabled.
// Fix: https://github.com/torvalds/linux/commit/82c387ef7568
//
// When NEED_RESCHED is set and interrupts are disabled, __cond_resched()
// should not call schedule(). The buggy kernel lacks the irqs_disabled()
// check, so schedule() runs and re-enables interrupts unexpectedly.

#include "driver.h"
#include "internal.h"
#include <linux/version.h>
#include <linux/irqflags.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 14, 0)

static struct task_struct *target;

static void setup(void) {
  target = kstep_task_create();
  kstep_task_pin(target, 1, 1);
}

static void run(void) {
  unsigned long flags;
  bool irqs_before, irqs_after;

  kstep_task_wakeup(target);
  kstep_tick_repeat(3);

  // Disable interrupts (mimics syscore_suspend/resume context)
  local_irq_save(flags);
  irqs_before = irqs_disabled();

  // Set NEED_RESCHED on the current task, simulating what happens when
  // a wakeup occurs during an interrupt-disabled section.
  set_tsk_need_resched(current);

  TRACE_INFO("irqs_disabled=%d need_resched=%d before cond_resched",
             irqs_before, test_tsk_need_resched(current));

  // On the buggy kernel, cond_resched() sees NEED_RESCHED and calls
  // schedule(), which re-enables interrupts. On the fixed kernel,
  // cond_resched() checks irqs_disabled() and returns early.
  cond_resched();

  irqs_after = irqs_disabled();
  TRACE_INFO("irqs_disabled=%d after cond_resched", irqs_after);

  if (irqs_before && !irqs_after) {
    kstep_fail("cond_resched enabled interrupts in irq-disabled context");
  } else if (irqs_before && irqs_after) {
    kstep_pass("cond_resched correctly skipped with irqs disabled");
  } else {
    TRACE_INFO("unexpected: irqs_before=%d irqs_after=%d",
               irqs_before, irqs_after);
  }

  local_irq_restore(flags);
}

KSTEP_DRIVER_DEFINE{
    .name = "cond_resched_irq",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#endif
