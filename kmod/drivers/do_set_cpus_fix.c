// https://github.com/torvalds/linux/commit/af13e5e437dc
//
// Bug: __balance_push_cpu_stop() calls select_fallback_rq() while holding
// rq->lock. select_fallback_rq() can call set_cpus_allowed_force(), which
// acquires __task_rq_lock (the same rq->lock) => recursive lock => deadlock.
//
// Fix: Move select_fallback_rq() before rq_lock(), calling it with only
// p->pi_lock held.
//
// Detection: Simulate the __balance_push_cpu_stop path by marking CPU 1 as
// inactive and scheduling the function on CPU 1's stopper thread. On the
// buggy kernel, the stopper thread deadlocks (task stays on CPU 1). On the
// fixed kernel, the task migrates off CPU 1.

#include "internal.h"
#include <linux/delay.h>
#include <linux/reboot.h>
#include <linux/stop_machine.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 18, 0)

KSYM_IMPORT_TYPED(struct cpumask, __cpu_active_mask);

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
}

static void run(void) {
  typedef int (*cpu_stop_fn_t)(void *);
  cpu_stop_fn_t bp_fn =
      (cpu_stop_fn_t)kstep_ksym_lookup("__balance_push_cpu_stop");
  if (!bp_fn) {
    kstep_fail("cannot resolve __balance_push_cpu_stop");
    return;
  }

  typedef bool (*stop_nowait_fn_t)(unsigned int, cpu_stop_fn_t, void *,
                                    struct cpu_stop_work *);
  stop_nowait_fn_t stop_fn =
      (stop_nowait_fn_t)kstep_ksym_lookup("stop_one_cpu_nowait");
  if (!stop_fn) {
    kstep_fail("cannot resolve stop_one_cpu_nowait");
    return;
  }

  kstep_task_pin(task, 1, 1);
  kstep_tick_repeat(10);

  int cpu_before = task_cpu(task);
  bool queued = task_on_rq_queued(task);
  TRACE_INFO("task pid=%d cpu=%d queued=%d", task->pid, cpu_before, queued);

  if (cpu_before != 1 || !queued) {
    kstep_fail("task not on CPU 1 (cpu=%d queued=%d)", cpu_before, queued);
    return;
  }

  // Mark CPU 1 as not active so is_cpu_allowed() returns false for it.
  // This simulates the CPU-hotplug teardown state where the dying CPU
  // is no longer in __cpu_active_mask.
  cpumask_clear_cpu(1, KSYM___cpu_active_mask);

  // __balance_push_cpu_stop does put_task_struct; take an extra ref.
  get_task_struct(task);

  // Schedule __balance_push_cpu_stop on CPU 1's stopper thread.
  struct cpu_stop_work work = {};
  stop_fn(1, bp_fn, task, &work);

  // Wait up to 3 s for the task to migrate off CPU 1.
  // On the buggy kernel the stopper thread deadlocks (irqs off) so
  // the task never moves.
  // NOTE: use mdelay (busy-wait) since kSTEP freezes jiffies.
  for (int i = 0; i < 300; i++) {
    if (task_cpu(task) != 1)
      break;
    mdelay(10);
  }

  // Restore active mask so cleanup can proceed.
  cpumask_set_cpu(1, KSYM___cpu_active_mask);

  int cpu_after = task_cpu(task);
  TRACE_INFO("after: task cpu=%d (was %d)", cpu_after, cpu_before);

  if (cpu_after != 1) {
    kstep_pass("task migrated to CPU %d - select_fallback_rq "
               "called without rq->lock (no deadlock)",
               cpu_after);
  } else {
    kstep_fail("task stuck on CPU 1 - select_fallback_rq "
               "called with rq->lock held (recursive lock deadlock)");
    // CPU 1 is deadlocked with irqs off; normal restart will hang.
    emergency_restart();
  }
}

#else
static void setup(void) {}
static void run(void) {
  kstep_pass("kernel version not applicable");
}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "do_set_cpus_fix",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
