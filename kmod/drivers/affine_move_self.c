// https://github.com/torvalds/linux/commit/9e81889c7648
//
// Bug: Two concurrent sched_setaffinity() calls on the same task can both
// read the same p->migration_pending pointer and issue stop_one_cpu_nowait()
// on it. This causes the same cpu_stop_work to be added to the stopper work
// queue twice, corrupting the stopper list.
//
// Fix: A new stop_pending boolean in set_affinity_pending tracks whether a
// stopper is in progress. Only the first caller enqueues the stopper.
//
// Reproduce: With PREEMPT_NONE, a spinning kthread on CPU 1 prevents the
// stopper thread from running. Two concurrent sched_setaffinity() calls on
// the target both enter affine_move_task() and (on the buggy kernel) both
// call stop_one_cpu_nowait() with the same stop_work. After yielding the
// target, the stopper processes a corrupted list, calling migration_cpu_stop
// repeatedly and underflowing the refcount, which triggers a WARN.

#include "driver.h"
#include "internal.h"
#include <linux/kthread.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 12, 0)

static struct task_struct *target;
static pid_t target_pid;

// Mirror kernel-internal struct from kernel/stop_machine.c
struct cpu_stopper_mirror {
  struct task_struct *thread;
  raw_spinlock_t lock;
  bool enabled;
  struct list_head works;
  // remaining fields omitted
};

static int setter_fn(void *data) {
  struct cpumask *mask = data;
  typedef long (*sa_fn_t)(pid_t, const struct cpumask *);
  sa_fn_t sa = (sa_fn_t)kstep_ksym_lookup("sched_setaffinity");
  if (!sa)
    return -1;
  sa(target_pid, mask);
  return 0;
}

static void setup(void) {
  target = kstep_kthread_create("target");
  kstep_kthread_bind(target, cpumask_of(1));
  kstep_kthread_start(target);
}

static void run(void) {
  kstep_tick_repeat(5);
  target_pid = target->pid;
  TRACE_INFO("target pid=%d cpu=%d", target->pid, task_cpu(target));

  static cpumask_t mask_a, mask_b;
  cpumask_clear(&mask_a);
  cpumask_set_cpu(2, &mask_a);
  cpumask_clear(&mask_b);
  cpumask_set_cpu(3, &mask_b);

  struct task_struct *setter_a =
      kthread_create(setter_fn, &mask_a, "setter_a");
  struct task_struct *setter_b =
      kthread_create(setter_fn, &mask_b, "setter_b");
  if (IS_ERR(setter_a) || IS_ERR(setter_b)) {
    kstep_fail("failed to create setter kthreads");
    return;
  }
  set_cpus_allowed_ptr(setter_a, cpumask_of(2));
  set_cpus_allowed_ptr(setter_b, cpumask_of(3));

  // Start both setters simultaneously. Both will call sched_setaffinity()
  // on the target. With PREEMPT_NONE and the target spinning on CPU 1,
  // the stopper cannot run, so both calls enqueue to the stopper queue
  // before any work is processed.
  wake_up_process(setter_a);
  wake_up_process(setter_b);

  // Wait for both setters to enter wait_for_completion
  kstep_tick_repeat(20);

  // Check stopper queue on CPU 1 for corruption.
  // Look up the cpu_stopper per-CPU variable and compute the per-CPU
  // instance for CPU 1 using __per_cpu_offset.
  void *stopper_base = kstep_ksym_lookup("cpu_stopper");
  if (!stopper_base) {
    kstep_fail("could not resolve cpu_stopper symbol");
    return;
  }
  struct cpu_stopper_mirror *stopper =
      (void *)stopper_base + __per_cpu_offset[1];

  // Count entries in the stopper queue. A self-loop (from double-enqueue
  // corruption) causes the iteration to exceed any normal count.
  int count = 0;
  unsigned long flags;
  struct list_head *pos;
  raw_spin_lock_irqsave(&stopper->lock, flags);
  list_for_each(pos, &stopper->works) {
    count++;
    if (count > 3)
      break;
  }
  raw_spin_unlock_irqrestore(&stopper->lock, flags);

  TRACE_INFO("stopper queue iteration count: %d (>3 = corrupted)", count);

  if (count > 2) {
    // Corrupted list detected (self-loop from double stop_one_cpu_nowait).
    // Fix the stopper queue to prevent infinite loop when the stopper runs.
    raw_spin_lock_irqsave(&stopper->lock, flags);
    INIT_LIST_HEAD(&stopper->works);
    raw_spin_unlock_irqrestore(&stopper->lock, flags);

    kstep_fail("stopper queue corrupted (iterated %d+ entries): "
               "concurrent sched_setaffinity double-enqueued stop_work",
               count);
  } else if (count == 1) {
    // Normal: only one stop_one_cpu_nowait enqueued (stop_pending prevented
    // the second). Yield target to let the stopper run and clean up.
    kstep_kthread_yield(target);
    kstep_tick_repeat(20);
    kstep_pass("stopper queue healthy (1 entry): "
               "stop_pending prevented double enqueue");
  } else {
    // Unexpected: 0 or 2 entries
    kstep_kthread_yield(target);
    kstep_tick_repeat(20);
    if (test_taint(TAINT_WARN))
      kstep_fail("WARN detected: stopper corruption from "
                 "concurrent sched_setaffinity");
    else
      kstep_pass("stopper queue has %d entries, no corruption detected",
                 count);
  }

  kstep_tick_repeat(10);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "affine_move_self",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
