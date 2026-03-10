// https://github.com/torvalds/linux/commit/df14b7f9efcd
//
// Bug: In __set_cpus_allowed_ptr_locked(), when the new cpumask equals the
// current cpus_mask, the code takes an early exit without updating
// user_cpus_ptr. This leaves user_cpus_ptr stale after a
// sched_setaffinity() call that doesn't change the effective mask.
//
// Fix: When SCA_USER is set, swap user_cpus_ptr with the new user mask
// even on the early-exit path.
//
// Reproduce: Pin a task to {1,2}, tamper with user_cpus_ptr to {1}, then
// pin again to {1,2} (same as cpus_mask). On buggy kernel user_cpus_ptr
// stays {1}; on fixed kernel it is swapped to {1,2}.

#include <linux/version.h>

#include "driver.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 2, 0)

static struct task_struct *task;

static void setup(void) { task = kstep_task_create(); }

static void run(void) {
  // Step 1: Pin task to CPUs 1-2 to allocate user_cpus_ptr
  kstep_task_pin(task, 1, 2);
  kstep_tick_repeat(5);

  if (!task->user_cpus_ptr) {
    kstep_fail("user_cpus_ptr not allocated after pin");
    return;
  }

  TRACE_INFO("after pin {1,2}: user_cpus_ptr=%*pbl cpus_mask=%*pbl",
             cpumask_pr_args(task->user_cpus_ptr),
             cpumask_pr_args(&task->cpus_mask));

  // Step 2: Tamper with user_cpus_ptr to clear CPU 2, making it {1}
  // This simulates user_cpus_ptr being out of date.
  cpumask_clear_cpu(2, task->user_cpus_ptr);

  TRACE_INFO("after tamper: user_cpus_ptr=%*pbl cpus_mask=%*pbl",
             cpumask_pr_args(task->user_cpus_ptr),
             cpumask_pr_args(&task->cpus_mask));

  // Step 3: Pin to {1,2} again — same as current cpus_mask
  // Buggy: early exit, user_cpus_ptr NOT swapped → stays {1}
  // Fixed: swap happens, user_cpus_ptr becomes {1,2}
  kstep_task_pin(task, 1, 2);
  kstep_tick_repeat(5);

  TRACE_INFO("after re-pin {1,2}: user_cpus_ptr=%*pbl cpus_mask=%*pbl",
             cpumask_pr_args(task->user_cpus_ptr),
             cpumask_pr_args(&task->cpus_mask));

  // Step 4: Check if user_cpus_ptr was updated
  bool has_cpu2 = cpumask_test_cpu(2, task->user_cpus_ptr);

  if (has_cpu2) {
    kstep_pass("user_cpus_ptr updated to {1,2} on re-pin with same mask");
  } else {
    kstep_fail("user_cpus_ptr stale {1} — missed update on early exit");
  }
}

KSTEP_DRIVER_DEFINE{
    .name = "missed_user_cpus_ptr",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#else
static void run(void) { TRACE_INFO("Skipped: wrong kernel version"); }
KSTEP_DRIVER_DEFINE{.name = "missed_user_cpus_ptr", .run = run};
#endif
