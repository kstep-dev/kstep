// https://github.com/torvalds/linux/commit/fd844ba9ae59
//
// Bug: __set_cpus_allowed_ptr() checks cpumask_equal(p->cpus_ptr, new_mask)
// instead of cpumask_equal(&p->cpus_mask, new_mask). When a task is
// migrate-disabled, cpus_ptr points to a temporary single-CPU mask, so if
// the new mask equals the current CPU, the check passes incorrectly and the
// actual cpus_mask update is skipped (lost).
//
// Fix: Compare against &p->cpus_mask (the long-term affinity) instead of
// p->cpus_ptr (the transitory mask during migrate-disable).
//
// Detection: Create a kthread on CPU 1 with wide affinity (CPU 0+1).
// Simulate migrate-disable by pointing cpus_ptr to a temp mask {CPU 1}.
// Call set_cpus_allowed_ptr() with {CPU 1}.
// On buggy: cpus_ptr == new_mask → early return → cpus_mask stays {0-1}.
// On fixed: cpus_mask != new_mask → proceeds → cpus_mask updated to {1}.

#include "internal.h"
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 8, 0)

static struct task_struct *kt;

static void setup(void) {
  kt = kstep_kthread_create("mask_test");
  // Bind to both CPUs so cpus_mask = {CPU0, CPU1}
  struct cpumask wide_mask;
  cpumask_clear(&wide_mask);
  cpumask_set_cpu(0, &wide_mask);
  cpumask_set_cpu(1, &wide_mask);
  kstep_kthread_bind(kt, &wide_mask);
  kstep_kthread_start(kt);
}

static void run(void) {
  // Let the kthread start running
  mdelay(50);

  TRACE_INFO("kthread pid=%d cpu=%d nr_cpus_allowed=%d",
             kt->pid, task_cpu(kt), kt->nr_cpus_allowed);
  TRACE_INFO("before: cpus_mask={%*pbl} cpus_ptr={%*pbl}",
             cpumask_pr_args(&kt->cpus_mask),
             cpumask_pr_args(kt->cpus_ptr));

  // Simulate migrate-disable: set cpus_ptr to a temp mask with only CPU 1
  cpumask_t temp_mask;
  cpumask_clear(&temp_mask);
  cpumask_set_cpu(1, &temp_mask);

  const cpumask_t *saved_cpus_ptr = kt->cpus_ptr;
  kt->cpus_ptr = &temp_mask;

  TRACE_INFO("simulated migrate-disable: cpus_ptr={%*pbl}",
             cpumask_pr_args(kt->cpus_ptr));

  // Try to set affinity to CPU 1 only.
  // Buggy kernel: cpumask_equal(cpus_ptr={1}, {1}) → true → skip update
  // Fixed kernel: cpumask_equal(cpus_mask={0-1}, {1}) → false → update
  struct cpumask new_mask;
  cpumask_clear(&new_mask);
  cpumask_set_cpu(1, &new_mask);
  set_cpus_allowed_ptr(kt, &new_mask);

  // Restore cpus_ptr before inspecting cpus_mask
  kt->cpus_ptr = saved_cpus_ptr;

  TRACE_INFO("after: cpus_mask={%*pbl} cpus_ptr={%*pbl} nr_cpus_allowed=%d",
             cpumask_pr_args(&kt->cpus_mask),
             cpumask_pr_args(kt->cpus_ptr), kt->nr_cpus_allowed);

  // Check: was cpus_mask updated to {CPU 1}?
  bool mask_updated = cpumask_equal(&kt->cpus_mask, &new_mask);

  if (!mask_updated) {
    kstep_fail("cpus_mask not updated to {1}: still {%*pbl} "
               "nr_cpus_allowed=%d - mask update lost (bug)",
               cpumask_pr_args(&kt->cpus_mask), kt->nr_cpus_allowed);
  } else {
    kstep_pass("cpus_mask correctly updated to {1}: nr_cpus_allowed=%d",
               kt->nr_cpus_allowed);
  }
}

#else
static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "cpus_mask_check",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
