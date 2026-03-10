// https://github.com/torvalds/linux/commit/248cc9993d1cc12b8e9ed716cc3fc09f6c3517dd
//
// Bug: cpuacct_charge() uses __this_cpu_add() which charges CPU time to the
// CPU executing the code, not the CPU where the task is running. During
// load_balance(), update_curr() is called from a different CPU, so cpuacct
// charges go to the wrong CPU.
//
// We directly call cpuacct_charge from CPU 2 for a task on CPU 1 and verify
// that the charge goes to the correct per-CPU bucket.

#include "driver.h"
#include "internal.h"
#include <linux/cgroup.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 17, 0)

// Mirror cpuacct struct from kernel/sched/cpuacct.c (v5.17)
struct kstep_cpuacct {
  struct cgroup_subsys_state css;
  u64 __percpu *cpuusage;
  struct kernel_cpustat __percpu *cpustat;
};

static struct task_struct *tasks[3];
static int balance_on_cpu2 = 0;
static int injected_charges = 0;

static void (*cpuacct_charge_fn)(struct task_struct *tsk, u64 cputime);

static u64 read_cpuacct_percpu(struct task_struct *tsk, int cpu) {
  struct cgroup_subsys_state *css = task_css(tsk, cpuacct_cgrp_id);
  struct kstep_cpuacct *ca = container_of(css, struct kstep_cpuacct, css);
  return *per_cpu_ptr(ca->cpuusage, cpu);
}

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();

  cpuacct_charge_fn = kstep_ksym_lookup("cpuacct_charge");
  if (!cpuacct_charge_fn)
    TRACE_INFO("WARNING: cpuacct_charge not found");
  else
    TRACE_INFO("cpuacct_charge found at %px", cpuacct_charge_fn);
}

// Called during load_balance on CPU 2 - simulate the bug condition by calling
// cpuacct_charge for a task pinned to CPU 1 from CPU 2's context
static void on_balance(int cpu, struct sched_domain *sd) {
  kstep_output_balance(cpu, sd);
  if (cpu == 2) {
    balance_on_cpu2++;
    if (cpuacct_charge_fn && injected_charges < 50) {
      cpuacct_charge_fn(tasks[0], 1000000);
      injected_charges++;
    }
  }
}

static void run(void) {
  // Pin tasks to CPU 1, leave CPU 2 idle to trigger load_balance
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], 1, 1);

  u64 cpu1_before = read_cpuacct_percpu(tasks[0], 1);
  u64 cpu2_before = read_cpuacct_percpu(tasks[0], 2);
  TRACE_INFO("Before: CPU1=%llu CPU2=%llu", cpu1_before, cpu2_before);

  kstep_tick_repeat(500);

  u64 cpu1_after = read_cpuacct_percpu(tasks[0], 1);
  u64 cpu2_after = read_cpuacct_percpu(tasks[0], 2);
  u64 cpu1_delta = cpu1_after - cpu1_before;
  u64 cpu2_delta = cpu2_after - cpu2_before;
  u64 injected_total = (u64)injected_charges * 1000000ULL;

  TRACE_INFO("After: CPU1 delta=%llu, CPU2 delta=%llu", cpu1_delta, cpu2_delta);
  TRACE_INFO("Balance on CPU2: %d, injected charges: %d (%llu ns)",
             balance_on_cpu2, injected_charges, injected_total);

  // cpuacct_charge was called from CPU 2 for a task on CPU 1.
  // Bug: __this_cpu_add charges to CPU 2 (the caller's CPU)
  // Fix: per_cpu_ptr(cpuusage, task_cpu(tsk)) charges to CPU 1 (task's CPU)
  if (cpu2_delta >= injected_total && injected_charges > 0) {
    kstep_fail("cpuacct_charge from CPU2 wrongly charged %llu ns to CPU2 "
               "(injected %llu ns for task on CPU1). "
               "CPU1 delta=%llu",
               cpu2_delta, injected_total, cpu1_delta);
  } else if (injected_charges > 0 && cpu2_delta < injected_total) {
    kstep_pass("cpuacct_charge correctly routed %d injected charges "
               "(%llu ns) to CPU1 (delta=%llu). CPU2 delta=%llu",
               injected_charges, injected_total, cpu1_delta, cpu2_delta);
  } else {
    kstep_pass("No injected charges (balance events: %d)", balance_on_cpu2);
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "charge_percpu_cpuusage",
    .setup = setup,
    .run = run,
    .on_sched_balance_selected = on_balance,
    .step_interval_us = 100,
};

#else
KSTEP_DRIVER_DEFINE{
    .name = "charge_percpu_cpuusage",
};
#endif
