// Reproducer for: sched: Fix migration_cpu_stop() requeueing
// Commit: 8a6edb5257e2
//
// Bug: In affine_move_task(), when called for a running task (not
// MIGRATE_ENABLE path), my_pending.arg is never initialized. If the task
// migrates before the stopper runs, migration_cpu_stop() finds task_rq(p)
// != rq, sees p->migration_pending is set, and requeues via
// stop_one_cpu_nowait(&pending->arg, ...). Since pending->arg is all zeros,
// the requeued call gets arg->task == NULL -> NULL pointer dereference.
//
// Fix: migration_cpu_stop() checks if arg->pending is NULL (meaning the call
// is from sched_exec/migrate_task_to, not affine_move_task). If so, it
// ignores p->migration_pending and does not attempt to requeue.
//
// Detection: Place a kthread on CPU 2, set migration_pending to a pending
// struct, change cpus_mask so CPU 2 is not allowed. Call migration_cpu_stop
// on CPU 1 with arg->pending = NULL. On the buggy kernel, it enters the
// requeue path (stop_one_cpu_nowait sets pending->stop_work.fn). On the
// fixed kernel, it ignores pending and exits cleanly.

#include "internal.h"
#include <linux/stop_machine.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 12, 0)

struct migration_arg {
  struct task_struct *task;
  int dest_cpu;
  struct set_affinity_pending *pending;
};

struct set_affinity_pending {
  refcount_t refs;
  struct completion done;
  struct cpu_stop_work stop_work;
  struct migration_arg arg;
};

static struct task_struct *victim;

static void setup(void) {
  victim = kstep_kthread_create("victim");
  kstep_kthread_bind(victim, cpumask_of(2));
  kstep_kthread_start(victim);
}

static void run(void) {
  kstep_tick_repeat(5);

  TRACE_INFO("victim pid=%d cpu=%d on_rq=%d",
             victim->pid, task_cpu(victim), victim->on_rq);

  typedef int (*cpu_stop_fn_t)(void *);
  cpu_stop_fn_t mcs =
      (cpu_stop_fn_t)kstep_ksym_lookup("migration_cpu_stop");
  typedef int (*stop_one_cpu_fn_t)(unsigned int, cpu_stop_fn_t, void *);
  stop_one_cpu_fn_t soc =
      (stop_one_cpu_fn_t)kstep_ksym_lookup("stop_one_cpu");

  if (!mcs || !soc) {
    kstep_fail("could not resolve kernel symbols");
    return;
  }

  // Create a set_affinity_pending, zero-initialized (mimics buggy
  // affine_move_task's "my_pending = { }").
  struct set_affinity_pending pending = {};
  refcount_set(&pending.refs, 3);
  init_completion(&pending.done);

  // Initialize pending->arg with valid data so if the buggy kernel
  // requeues, the second migration_cpu_stop call won't crash.
  pending.arg.task = victim;
  pending.arg.dest_cpu = task_cpu(victim);
  pending.arg.pending = &pending;

  // Install migration_pending on the victim
  victim->migration_pending = &pending;

  // Change victim's cpus_mask: remove CPU 2, add CPU 1.
  // This makes cpumask_test_cpu(task_cpu(p)=2, cpus_ptr) return false,
  // preventing the early exit in migration_cpu_stop's else-if branch.
  cpumask_t saved_mask;
  cpumask_copy(&saved_mask, &victim->cpus_mask);
  cpumask_clear(&victim->cpus_mask);
  cpumask_set_cpu(1, &victim->cpus_mask);

  // First-call arg: simulates a call from sched_exec/migrate_task_to.
  // arg->pending = NULL is the key: fixed kernel uses this to ignore
  // p->migration_pending.
  struct migration_arg arg = {
      .task = victim,
      .dest_cpu = 1,
      .pending = NULL,
  };

  TRACE_INFO("calling migration_cpu_stop on CPU 1 stopper");
  int ret = soc(1, mcs, &arg);
  TRACE_INFO("migration_cpu_stop returned %d", ret);

  // Detect if requeue was attempted by checking stop_work.fn.
  // stop_one_cpu_nowait sets this; it stays NULL if never called.
  bool requeued = (pending.stop_work.fn != NULL);
  TRACE_INFO("pending.stop_work.fn = %px (requeued=%d)",
             pending.stop_work.fn, requeued);

  // Let any requeued stopper work complete safely
  kstep_tick_repeat(10);

  // Restore state
  cpumask_copy(&victim->cpus_mask, &saved_mask);
  victim->migration_pending = NULL;

  if (requeued)
    kstep_fail("migration_cpu_stop requeued with pending->arg "
               "(uninitialized in real buggy code path)");
  else
    kstep_pass("migration_cpu_stop correctly ignored pending "
               "when arg->pending == NULL");
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "migration_cpu_stop_requeue",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
