// https://github.com/torvalds/linux/commit/234a503e670b
//
// Bug: __set_cpus_allowed_ptr() does not validate that the requested affinity
// mask is a subset of task_cpu_possible_mask(). This allows setting a task's
// cpus_mask to include CPUs it cannot execute on.
//
// Fix: Add cpumask_subset(new_mask, task_cpu_possible_mask(p)) check that
// returns -EINVAL for non-kthread tasks with invalid masks.

#include <linux/version.h>
#include <linux/cpumask.h>
#include <linux/sched.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 13, 0)

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
}

static void run(void) {
  int ret;
  int num_possible = num_possible_cpus();
  int impossible_cpu = num_possible; // first CPU beyond cpu_possible_mask

  TRACE_INFO("System has %d possible CPUs, using CPU %d as impossible",
             num_possible, impossible_cpu);

  // Pin task to CPU 1 and wake it
  kstep_task_pin(task, 1, 1);
  kstep_task_wakeup(task);
  kstep_tick_repeat(3);

  TRACE_INFO("Task %d pinned to CPU 1, current mask: %*pbl",
             task->pid, cpumask_pr_args(task->cpus_ptr));

  // Build a mask that includes CPU 1 (valid) and an impossible CPU
  cpumask_var_t bad_mask;
  if (!alloc_cpumask_var(&bad_mask, GFP_KERNEL))
    panic("Failed to allocate cpumask");

  cpumask_clear(bad_mask);
  cpumask_set_cpu(1, bad_mask);
  cpumask_set_cpu(impossible_cpu, bad_mask);

  TRACE_INFO("Requesting affinity mask: %*pbl (includes impossible CPU %d)",
             cpumask_pr_args(bad_mask), impossible_cpu);
#ifdef task_cpu_possible_mask
  TRACE_INFO("task_cpu_possible_mask: %*pbl",
             cpumask_pr_args(task_cpu_possible_mask(task)));
#endif

  // Call set_cpus_allowed_ptr with the invalid mask
  // Buggy kernel: returns 0 (success), sets cpus_mask to include impossible CPU
  // Fixed kernel: returns -EINVAL, rejects the change
  ret = set_cpus_allowed_ptr(task, bad_mask);

  TRACE_INFO("set_cpus_allowed_ptr returned %d", ret);
  TRACE_INFO("Task cpus_mask after call: %*pbl",
             cpumask_pr_args(task->cpus_ptr));

  if (ret == 0) {
    // Check if the impossible CPU is now in the task's mask
    if (cpumask_test_cpu(impossible_cpu, task->cpus_ptr)) {
      kstep_fail("BUG: task cpus_mask contains impossible CPU %d "
                 "(not in cpu_possible_mask)", impossible_cpu);
    } else {
      kstep_pass("affinity set succeeded but impossible CPU not in mask");
    }
  } else if (ret == -EINVAL) {
    kstep_pass("affinity change correctly rejected with -EINVAL");
  } else {
    TRACE_INFO("Unexpected return value %d", ret);
  }

  free_cpumask_var(bad_mask);
  kstep_tick_repeat(3);
}

static void on_tick_begin(void) { kstep_output_curr_task(); }

KSTEP_DRIVER_DEFINE{
    .name = "reject_affinity",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 10000,
};

#else
static void run(void) { TRACE_INFO("Skipped: wrong kernel version"); }
KSTEP_DRIVER_DEFINE{.name = "reject_affinity", .run = run};
#endif
