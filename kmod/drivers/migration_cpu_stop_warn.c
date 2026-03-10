// https://github.com/torvalds/linux/commit/1293771e4353c148d5f6908fb32d1c1cfd653e47
//
// Bug: migration_cpu_stop() uses is_cpu_allowed(p, cpu_of(rq)) where rq is
// the stopper's CPU, not the task's actual CPU. When the task has moved to a
// different CPU, this causes a spurious WARN.
//
// Reproduce: Call migration_cpu_stop on CPU 1's stopper with a task that is
// pinned to CPU 2. Since task_rq(p) != rq (task on CPU 2, stopper on CPU 1),
// it enters the else-if branch with dest_cpu < 0 and !pending, which checks
// is_cpu_allowed(p, cpu_of(rq)) = is_cpu_allowed(p, 1). Since CPU 1 is not
// in the task's cpus_mask, the WARN fires.

#include "internal.h"
#include <linux/stop_machine.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 10, 0)

// Mirror the kernel-internal struct from kernel/sched/core.c
struct migration_arg {
  struct task_struct *task;
  int dest_cpu;
  struct set_affinity_pending *pending;
};

static struct task_struct *victim;

static void setup(void) {
  victim = kstep_kthread_create("victim");
  kstep_kthread_bind(victim, cpumask_of(2));
  kstep_kthread_start(victim);
}

static void run(void) {
  // Let the kthread settle on CPU 2
  kstep_tick_repeat(5);

  TRACE_INFO("victim pid=%d cpu=%d on_rq=%d",
             victim->pid, task_cpu(victim), victim->on_rq);

  // Look up the static migration_cpu_stop function
  typedef int (*cpu_stop_fn_t)(void *);
  cpu_stop_fn_t mcs =
      (cpu_stop_fn_t)kstep_ksym_lookup("migration_cpu_stop");
  if (!mcs) {
    kstep_fail("could not resolve migration_cpu_stop");
    return;
  }

  // Ensure victim's migration_pending is NULL so we hit the !pending branch
#if defined(CONFIG_SMP) && LINUX_VERSION_CODE > KERNEL_VERSION(5, 10, 0)
  TRACE_INFO("migration_pending=%px", victim->migration_pending);
#endif

  // Craft a migration_arg that simulates a migrate_enable() path:
  //   dest_cpu = -1 (migrate_enable), pending = NULL
  struct migration_arg arg = {
      .task = victim,
      .dest_cpu = -1,
      .pending = NULL,
  };

  // Run migration_cpu_stop on CPU 1's stopper thread.
  // The task is on CPU 2, so task_rq(p) != this_rq() on CPU 1.
  // With dest_cpu < 0 and !pending, the buggy code checks:
  //   is_cpu_allowed(p, cpu_of(rq)) = is_cpu_allowed(p, 1) -> false -> WARN!
  // The fix checks:
  //   cpumask_test_cpu(task_cpu(p), &p->cpus_mask) -> true -> no WARN
  TRACE_INFO("calling migration_cpu_stop on CPU 1 stopper");
  typedef int (*stop_one_cpu_fn_t)(unsigned int, cpu_stop_fn_t, void *);
  stop_one_cpu_fn_t stop_one_cpu_fn =
      (stop_one_cpu_fn_t)kstep_ksym_lookup("stop_one_cpu");
  if (!stop_one_cpu_fn) {
    kstep_fail("could not resolve stop_one_cpu");
    return;
  }
  int ret = stop_one_cpu_fn(1, mcs, &arg);
  TRACE_INFO("migration_cpu_stop returned %d", ret);

  // WARN_ON_ONCE taints the kernel with TAINT_WARN
  if (test_taint(TAINT_WARN))
    kstep_fail("WARN_ON_ONCE fired in migration_cpu_stop: "
               "is_cpu_allowed used wrong cpu_of(rq)");
  else
    kstep_pass("no WARN fired: task_cpu(p) in cpus_mask checked correctly");
}

KSTEP_DRIVER_DEFINE{
    .name = "migration_cpu_stop_warn",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
KSTEP_DRIVER_DEFINE{
    .name = "migration_cpu_stop_warn",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
#endif
