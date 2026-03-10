// https://github.com/torvalds/linux/commit/e0b257c3b71b
//
// Bug: trigger_load_balance() does not check cpu_active() before raising
// SCHED_SOFTIRQ. When a CPU is deactivated (cleared from cpu_active_mask)
// but not yet offline, scheduler_tick() -> trigger_load_balance() still
// raises SCHED_SOFTIRQ on that CPU. This can cause warnings in NOHZ code
// when stopping the tick on an inactive CPU.
//
// Fix: Add !cpu_active(cpu_of(rq)) check to trigger_load_balance() so it
// returns early without raising SCHED_SOFTIRQ on inactive CPUs.
//
// Reproduce: Clear CPU 1 from cpu_active_mask (simulating CPU teardown),
// then tick. On buggy kernel, SCHED_SOFTIRQ fires on the inactive CPU.
// On fixed kernel, trigger_load_balance returns early.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 11, 0)

KSYM_IMPORT_TYPED(struct cpumask, __cpu_active_mask);

static struct task_struct *task;
static int softirq_on_inactive;

static void on_softirq_begin(void) {
  int cpu = smp_processor_id();
  if (!cpumask_test_cpu(cpu, KSYM___cpu_active_mask)) {
    softirq_on_inactive++;
    TRACE_INFO("SCHED_SOFTIRQ raised on inactive CPU %d!", cpu);
  }
}

static void setup(void) {
  task = kstep_task_create();
  kstep_task_pin(task, 1, 1);
}

static void run(void) {
  // Settle tasks and advance time so rq->next_balance is reached
  kstep_tick_repeat(20);

  struct rq *rq1 = cpu_rq(1);
  TRACE_INFO("CPU 1: next_balance=%lu jiffies=%lu active=%d",
             rq1->next_balance, jiffies, cpumask_test_cpu(1, KSYM___cpu_active_mask));

  // Clear CPU 1 from active mask (simulate CPU deactivation)
  cpumask_clear_cpu(1, KSYM___cpu_active_mask);
  TRACE_INFO("CPU 1 deactivated: active=%d",
             cpumask_test_cpu(1, KSYM___cpu_active_mask));

  // Ensure rq->next_balance <= jiffies so trigger_load_balance raises softirq
  rq1->next_balance = jiffies;

  softirq_on_inactive = 0;

  // Tick again - on buggy kernel, trigger_load_balance will raise
  // SCHED_SOFTIRQ on inactive CPU 1
  kstep_tick_repeat(5);

  // Restore active mask
  cpumask_set_cpu(1, KSYM___cpu_active_mask);

  TRACE_INFO("softirq_on_inactive=%d", softirq_on_inactive);

  if (softirq_on_inactive > 0) {
    kstep_fail("SCHED_SOFTIRQ raised %d times on inactive CPU 1 - "
               "trigger_load_balance missing cpu_active check",
               softirq_on_inactive);
  } else {
    kstep_pass("SCHED_SOFTIRQ not raised on inactive CPU 1 - "
               "trigger_load_balance correctly checks cpu_active");
  }
}

#else
static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}
static void on_softirq_begin(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "softirq_inactive_cpu",
    .setup = setup,
    .run = run,
    .on_sched_softirq_begin = on_softirq_begin,
    .step_interval_us = 10000,
    .tick_interval_ns = 4000000,
};
