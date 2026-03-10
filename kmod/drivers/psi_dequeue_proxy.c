// https://github.com/torvalds/linux/commit/c2ae8b0df2d1
//
// Bug: psi_dequeue() with DEQUEUE_SLEEP unconditionally returns early,
// assuming psi_task_switch() will clear TSK_RUNNING shortly after.
// With Proxy Execution, a mutex-blocked task can be switched off the CPU
// (TSK_ONCPU cleared) while retaining TSK_RUNNING on the runqueue. When
// the task is later dequeued with DEQUEUE_SLEEP, psi_dequeue() skips
// clearing TSK_RUNNING. On re-enqueue, psi_enqueue() tries to set
// TSK_RUNNING again, triggering "psi: inconsistent task state!".
//
// Fix: Also check (p->psi_flags & TSK_ONCPU) before the early return.
//
// Reproduction: Start a kthread on CPU 1 so it gets psi_flags = TSK_RUNNING |
// TSK_ONCPU. Then use psi_task_change to clear TSK_ONCPU (simulating the proxy
// execution switch-off). Now dequeue with DEQUEUE_SLEEP. On buggy kernel,
// TSK_RUNNING stays set; on fixed, it is cleared. Waking the task on the buggy
// kernel triggers the "psi: inconsistent task state!" error.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

#ifdef CONFIG_PSI

static struct task_struct *kt_target;

static void setup(void) {
  kt_target = kstep_kthread_create("psi_tgt");
  kstep_kthread_bind(kt_target, cpumask_of(1));
}

static void run(void) {
  typedef bool (*dequeue_task_fn)(struct rq *, struct task_struct *, int);
  typedef void (*psi_task_change_fn)(struct task_struct *, int, int);

  dequeue_task_fn ksym_dequeue_task =
      (dequeue_task_fn)kstep_ksym_lookup("dequeue_task");
  psi_task_change_fn ksym_psi_task_change =
      (psi_task_change_fn)kstep_ksym_lookup("psi_task_change");

  if (!ksym_dequeue_task || !ksym_psi_task_change) {
    kstep_fail("cannot resolve symbols: dequeue_task=%p psi_task_change=%p",
               ksym_dequeue_task, ksym_psi_task_change);
    return;
  }

  // Step 1: Start target kthread on CPU 1
  kstep_kthread_start(kt_target);
  kstep_tick_repeat(5);

  TRACE_INFO("step1 target: psi_flags=0x%x on_rq=%d cpu=%d",
             kt_target->psi_flags, kt_target->on_rq, task_cpu(kt_target));

  if (!(kt_target->psi_flags & TSK_ONCPU)) {
    kstep_fail("target missing TSK_ONCPU after start (psi_flags=0x%x)",
               kt_target->psi_flags);
    return;
  }

  // Step 2: Simulate proxy execution switch-off by clearing TSK_ONCPU.
  // In real proxy execution, psi_task_switch() clears TSK_ONCPU when the
  // blocked task is switched off CPU but stays on the runqueue.
  ksym_psi_task_change(kt_target, TSK_ONCPU, 0);

  TRACE_INFO("step2 after clearing TSK_ONCPU: psi_flags=0x%x",
             kt_target->psi_flags);

  if (!(kt_target->psi_flags & TSK_RUNNING)) {
    kstep_fail("target missing TSK_RUNNING (psi_flags=0x%x)",
               kt_target->psi_flags);
    return;
  }
  if (kt_target->psi_flags & TSK_ONCPU) {
    kstep_fail("TSK_ONCPU not cleared (psi_flags=0x%x)",
               kt_target->psi_flags);
    return;
  }

  // Step 3: Dequeue with DEQUEUE_SLEEP (simulating proxy_deactivate).
  // On buggy kernel: psi_dequeue sees DEQUEUE_SLEEP → returns early →
  //   TSK_RUNNING stays set.
  // On fixed kernel: psi_dequeue sees DEQUEUE_SLEEP but no TSK_ONCPU →
  //   proceeds to clear all psi_flags.
  struct rq *rq = cpu_rq(1);
  unsigned long flags;
  raw_spin_lock_irqsave(&rq->__lock, flags);

  WRITE_ONCE(kt_target->__state, TASK_UNINTERRUPTIBLE);
  ksym_dequeue_task(rq, kt_target, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);

  unsigned int psi_after = kt_target->psi_flags;

  raw_spin_unlock_irqrestore(&rq->__lock, flags);

  TRACE_INFO("step3 after DEQUEUE_SLEEP: psi_flags=0x%x (expect buggy=0x%x fixed=0x0)",
             psi_after, TSK_RUNNING);

  // Step 4: Wake the task. On buggy kernel this triggers
  // "psi: inconsistent task state!" because psi_enqueue tries to set
  // TSK_RUNNING on a task that already has it.
  wake_up_process(kt_target);
  kstep_tick_repeat(5);

  TRACE_INFO("step4 after wake: psi_flags=0x%x", kt_target->psi_flags);

  if (psi_after & TSK_RUNNING) {
    kstep_fail("psi_dequeue did not clear TSK_RUNNING on DEQUEUE_SLEEP "
               "without TSK_ONCPU (psi_flags=0x%x after dequeue)",
               psi_after);
  } else {
    kstep_pass("psi_dequeue correctly cleared flags (psi_flags=0x%x)",
               psi_after);
  }
}

#else /* !CONFIG_PSI */

static void setup(void) {}
static void run(void) {
  kstep_fail("CONFIG_PSI not enabled");
}

#endif /* CONFIG_PSI */

#else /* wrong kernel version */

static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}

#endif

KSTEP_DRIVER_DEFINE{
    .name = "psi_dequeue_proxy",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
