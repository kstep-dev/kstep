// https://github.com/torvalds/linux/commit/248cc9993d1cc12b8e9ed716cc3fc09f6c3517dd
//
// Bug: cpuacct_charge() uses __this_cpu_add() which charges CPU time to the
// CPU executing the code, not the CPU where the task is running. During
// load_balance(), update_curr() is called from a different CPU, so cpuacct
// charges go to the wrong CPU.

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

static u64 read_cpuacct_percpu(struct task_struct *tsk, int cpu) {
  struct cgroup_subsys_state *css = task_css(tsk, cpuacct_cgrp_id);
  struct kstep_cpuacct *ca =
      container_of(css, struct kstep_cpuacct, css);
  return *per_cpu_ptr(ca->cpuusage, cpu);
}

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void on_balance(int cpu, struct sched_domain *sd) {
  kstep_output_balance(cpu, sd);
  if (cpu == 2)
    balance_on_cpu2++;
}

static void run(void) {
  // Pin all tasks to CPU 1, leaving CPU 2 idle to trigger load_balance
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], 1, 1);

  // Record baseline cpuacct per-CPU usage for this cgroup
  u64 cpu1_before = read_cpuacct_percpu(tasks[0], 1);
  u64 cpu2_before = read_cpuacct_percpu(tasks[0], 2);
  TRACE_INFO("Before: CPU1=%llu CPU2=%llu", cpu1_before, cpu2_before);

  // Tick to accumulate runtime and trigger load balancing from idle CPU 2
  kstep_tick_repeat(500);

  u64 cpu1_after = read_cpuacct_percpu(tasks[0], 1);
  u64 cpu2_after = read_cpuacct_percpu(tasks[0], 2);
  u64 cpu1_delta = cpu1_after - cpu1_before;
  u64 cpu2_delta = cpu2_after - cpu2_before;

  TRACE_INFO("After: CPU1=%llu (delta=%llu) CPU2=%llu (delta=%llu)",
             cpu1_after, cpu1_delta, cpu2_after, cpu2_delta);
  TRACE_INFO("Balance events on CPU2: %d", balance_on_cpu2);

  // All tasks run on CPU 1, so CPU 2 cpuacct delta should be zero.
  // Bug: __this_cpu_add charges to CPU 2 during load_balance from CPU 2.
  if (cpu2_delta > 0 && balance_on_cpu2 > 0) {
    kstep_fail("cpuacct wrongly charged %llu ns to CPU2 "
               "(tasks only on CPU1, delta=%llu). "
               "Balance events on CPU2: %d",
               cpu2_delta, cpu1_delta, balance_on_cpu2);
  } else if (cpu2_delta == 0) {
    kstep_pass("cpuacct correctly charged all time to CPU1 (%llu ns). "
               "CPU2 delta=0. Balance events on CPU2: %d",
               cpu1_delta, balance_on_cpu2);
  } else {
    kstep_pass("cpu2_delta=%llu but no balance on CPU2 (%d events)",
               cpu2_delta, balance_on_cpu2);
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
