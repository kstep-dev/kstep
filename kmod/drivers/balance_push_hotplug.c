// https://github.com/torvalds/linux/commit/b5c4477366fb
//
// Bug: balance_push() does not check cpu_dying(), so when
// balance_push_callback is left installed (e.g. from stale hotplug state
// after a failed cpu_down rollback), it incorrectly pushes regular kernel
// threads off a CPU that is not actually dying.
//
// Fix: Gate balance_push() with cpu_dying(rq->cpu), so it returns early
// when the CPU is not in a dying state.
//
// Reproduce: Install balance_push_callback on CPU 1 while the CPU is
// fully online and not dying. Create a kthread running on CPU 1 with
// all-CPUs affinity. Tick to trigger balance_push callback.
//   Buggy kernel: balance_push() pushes the kthread off CPU 1.
//   Fixed kernel: balance_push() checks cpu_dying(1) -> false -> returns
//   early, kthread stays.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 12, 0)

KSYM_IMPORT(balance_push_callback);

static struct task_struct *kt;

static void setup(void) {
  kt = kstep_kthread_create("bp_test");
  kstep_kthread_bind(kt, cpumask_of(1));
  kstep_kthread_start(kt);
}

static void run(void) {
  struct rq *rq1 = cpu_rq(1);
  unsigned long flags;

  // Let kthread settle on CPU 1
  kstep_tick_repeat(5);
  TRACE_INFO("kthread pid=%d cpu=%d before push", kt->pid, task_cpu(kt));

  // Expand affinity so select_fallback_rq can pick another CPU
  set_cpus_allowed_ptr(kt, cpu_possible_mask);

  // Install balance_push_callback on CPU 1's rq.
  // This simulates the stale callback left after a failed hotplug rollback
  // where sched_cpu_deactivate ran but cpu_down() failed and rolled back.
  raw_spin_lock_irqsave(&rq1->lock, flags);
  rq1->balance_callback = KSYM_balance_push_callback;
  raw_spin_unlock_irqrestore(&rq1->lock, flags);

  TRACE_INFO("installed balance_push_callback on CPU 1 (cpu_dying=%d)",
             cpu_dying(1));

  // Make the kthread yield so it calls schedule(), which processes
  // balance callbacks including balance_push.
  kstep_kthread_yield(kt);
  kstep_tick_repeat(20);

  int final_cpu = task_cpu(kt);
  TRACE_INFO("kthread cpu=%d after balance_push ticks", final_cpu);

  if (final_cpu != 1) {
    kstep_fail("kthread pushed from CPU 1 to CPU %d: "
               "balance_push active despite CPU not dying "
               "(stale balance_push_callback)", final_cpu);
  } else {
    kstep_pass("kthread stayed on CPU 1: "
               "balance_push correctly gated by cpu_dying()");
  }

  // Cleanup: remove balance_push callback from rq
  raw_spin_lock_irqsave(&rq1->lock, flags);
  if (rq1->balance_callback == KSYM_balance_push_callback)
    rq1->balance_callback = NULL;
  raw_spin_unlock_irqrestore(&rq1->lock, flags);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "balance_push_hotplug",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
