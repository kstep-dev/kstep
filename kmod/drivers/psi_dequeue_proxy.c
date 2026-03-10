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
// Reproduction: Create two kthreads on CPU 1 (CFS target + FIFO preemptor).
// Once the FIFO kthread preempts the CFS kthread, the CFS kthread has
// psi_flags = TSK_RUNNING (no TSK_ONCPU). Then dequeue the CFS kthread
// with DEQUEUE_SLEEP. On buggy kernel, TSK_RUNNING stays set; on fixed,
// it is cleared. Waking the task on the buggy kernel triggers the error.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

#ifdef CONFIG_PSI

static struct task_struct *kt_target, *kt_preemptor;

static void setup(void) {
  kt_target = kstep_kthread_create("psi_tgt");
  kstep_kthread_bind(kt_target, cpumask_of(1));

  kt_preemptor = kstep_kthread_create("psi_hog");
  kstep_kthread_bind(kt_preemptor, cpumask_of(1));
}

static void run(void) {
  typedef bool (*dequeue_task_fn)(struct rq *, struct task_struct *, int);
  dequeue_task_fn ksym_dequeue_task =
      (dequeue_task_fn)kstep_ksym_lookup("dequeue_task");
  if (!ksym_dequeue_task) {
    kstep_fail("cannot resolve dequeue_task");
    return;
  }

  // Step 1: Start target CFS kthread on CPU 1
  kstep_kthread_start(kt_target);
  kstep_tick_repeat(5);

  TRACE_INFO("target initial: psi_flags=0x%x on_rq=%d cpu=%d",
             kt_target->psi_flags, kt_target->on_rq, task_cpu(kt_target));

  // Step 2: Start FIFO preemptor to preempt target
  struct sched_param sp = {.sched_priority = 80};
  sched_setscheduler_nocheck(kt_preemptor, SCHED_FIFO, &sp);
  kstep_kthread_start(kt_preemptor);
  kstep_tick_repeat(5);

  TRACE_INFO("target after preempt: psi_flags=0x%x on_rq=%d cpu=%d",
             kt_target->psi_flags, kt_target->on_rq, task_cpu(kt_target));

  // Verify target has TSK_RUNNING but not TSK_ONCPU
  if (!(kt_target->psi_flags & TSK_RUNNING)) {
    kstep_fail("target missing TSK_RUNNING (psi_flags=0x%x)",
               kt_target->psi_flags);
    return;
  }
  if (kt_target->psi_flags & TSK_ONCPU) {
    kstep_fail("target still has TSK_ONCPU (psi_flags=0x%x)",
               kt_target->psi_flags);
    return;
  }
  if (!task_on_rq_queued(kt_target)) {
    kstep_fail("target not on runqueue");
    return;
  }

  // Step 3: Simulate proxy-execution dequeue.
  // Set state to TASK_UNINTERRUPTIBLE (as if blocked on a mutex)
  // then dequeue with DEQUEUE_SLEEP.
  struct rq_flags rf;
  struct rq *rq = task_rq_lock(kt_target, &rf);

  WRITE_ONCE(kt_target->__state, TASK_UNINTERRUPTIBLE);
  ksym_dequeue_task(rq, kt_target, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
  WRITE_ONCE(kt_target->on_rq, 0);

  unsigned int psi_after = kt_target->psi_flags;

  task_rq_unlock(rq, kt_target, &rf);

  TRACE_INFO("after DEQUEUE_SLEEP: psi_flags=0x%x (buggy=0x%x fixed=0x0)",
             psi_after, TSK_RUNNING);

  // Step 4: Wake the task — on buggy kernel this triggers
  // "psi: inconsistent task state!" because TSK_RUNNING is already set
  wake_up_process(kt_target);
  kstep_tick_repeat(5);

  TRACE_INFO("after wake: psi_flags=0x%x", kt_target->psi_flags);

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
