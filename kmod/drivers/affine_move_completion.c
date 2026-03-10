// https://github.com/torvalds/linux/commit/d707faa64d03
//
// Bug: affine_move_task() hangs on wait_for_completion() because
// migration_cpu_stop() fails to signal completion when the task moves to
// an allowed CPU between scheduling the stopper and the stopper running.
//
// In migration_cpu_stop(), when task_rq(p) != rq and dest_cpu >= 0,
// the else-if (dest_cpu < 0) branch was not taken, so complete_all()
// was never called. The caller hangs forever on wait_for_completion().
//
// Fix: Change the condition to (dest_cpu < 0 || pending) so that pending
// affinity requests are always handled, even when the task has moved.
//
// Reproduce: Create a kthread on CPU 2, set migration_pending, then
// schedule migration_cpu_stop on CPU 1 via stop_one_cpu_nowait. Since
// task_rq(target) = CPU 2's rq != CPU 1's rq and dest_cpu >= 0, the
// buggy kernel skips completion. The fixed kernel enters the else-if
// branch and completes the pending request.

#include "driver.h"
#include "internal.h"
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/stop_machine.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 10, 0)

struct migration_arg_k {
  struct task_struct *task;
  int dest_cpu;
  struct set_affinity_pending *pending;
};

struct set_affinity_pending_k {
  refcount_t refs;
  struct completion done;
  struct cpu_stop_work stop_work;
  struct migration_arg_k arg;
};

static struct task_struct *target;

static void setup(void) {
  target = kstep_kthread_create("target");
  kstep_kthread_bind(target, cpumask_of(2));
  kstep_kthread_start(target);
}

static void run(void) {
  kstep_tick_repeat(5);
  TRACE_INFO("target pid=%d cpu=%d", target->pid, task_cpu(target));

  if (task_cpu(target) != 2) {
    kstep_fail("target not on CPU 2 (cpu=%d)", task_cpu(target));
    return;
  }

  typedef int (*cpu_stop_fn_t)(void *);
  cpu_stop_fn_t mcs_fn =
      (cpu_stop_fn_t)kstep_ksym_lookup("migration_cpu_stop");
  if (!mcs_fn) {
    kstep_fail("cannot resolve migration_cpu_stop");
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

  // Set up a set_affinity_pending to simulate the state left by
  // __set_cpus_allowed_ptr() + affine_move_task(): a pending migration
  // request that uses stop_one_cpu on the task's original CPU.
  static struct set_affinity_pending_k pend;
  memset(&pend, 0, sizeof(pend));
  refcount_set(&pend.refs, 2);
  init_completion(&pend.done);

  // Install migration_pending on the target under pi_lock.
  // This simulates: __set_cpus_allowed_ptr changed cpus_mask to {CPU 2}
  // and affine_move_task installed a pending on the task.
  raw_spin_lock_irq(&target->pi_lock);
  target->migration_pending = (struct set_affinity_pending *)&pend;
  raw_spin_unlock_irq(&target->pi_lock);

  // migration_arg: task on CPU 2, dest_cpu = 2 (>= 0).
  // The arg.pending is NULL, matching the task_running path where
  // affine_move_task uses a local arg without pending pointer.
  static struct migration_arg_k arg;
  arg.task = target;
  arg.dest_cpu = 2;
  arg.pending = NULL;

  // Schedule migration_cpu_stop on CPU 1's stopper thread.
  // Since the task is on CPU 2, task_rq(target) != this_rq() (CPU 1).
  //
  // Buggy kernel: else-if (dest_cpu < 0) is false (dest_cpu = 2),
  //   falls through to out: with complete = false. No complete_all().
  //
  // Fixed kernel: else-if (dest_cpu < 0 || pending) is true because
  //   pending != NULL. Then cpumask_test_cpu(task_cpu(p), p->cpus_ptr)
  //   is true (task on CPU 2, mask = {CPU 2}), so it completes.
  static struct cpu_stop_work stop_work;
  memset(&stop_work, 0, sizeof(stop_work));

  TRACE_INFO("scheduling migration_cpu_stop on CPU 1 (task on CPU 2)");
  stop_fn(1, mcs_fn, &arg, &stop_work);

  // Advance ticks to let the stopper thread run on CPU 1.
  kstep_tick_repeat(20);
  mdelay(100);
  kstep_tick_repeat(20);

  // Check if the completion was signaled.
  bool completed = try_wait_for_completion(&pend.done);
  TRACE_INFO("completion signaled: %s", completed ? "YES" : "NO");

  // Clean up: ensure migration_pending is cleared.
  raw_spin_lock_irq(&target->pi_lock);
  if (target->migration_pending ==
      (struct set_affinity_pending *)&pend)
    target->migration_pending = NULL;
  raw_spin_unlock_irq(&target->pi_lock);

  if (!completed) {
    kstep_fail("migration_pending not completed: task would hang "
               "forever in wait_for_completion (missing completion "
               "for affine_move_task waiter)");
  } else {
    kstep_pass("migration_pending completed: race condition handled "
               "correctly");
  }

  kstep_tick_repeat(5);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "affine_move_completion",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
