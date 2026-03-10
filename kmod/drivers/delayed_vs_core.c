// https://github.com/torvalds/linux/commit/c662e2b1e8cf
//
// Bug: sched_core_enqueue() and sched_core_dequeue() do not check the
// se.sched_delayed flag. When a task in delayed-dequeue state undergoes a
// dequeue/enqueue cycle (e.g. migration), sched_core_enqueue() adds it to
// the core_tree despite being logically not runnable. This breaks the
// invariant that sched_delayed tasks should not be in the core scheduling
// tree.
//
// Fix: Add early return in sched_core_enqueue() and sched_core_dequeue()
// when p->se.sched_delayed is set.
//
// Test: Enable core scheduling, give a task a core cookie, remove it from
// core_tree, set sched_delayed = 1, then call sched_core_enqueue().
// On buggy kernel: task is added to core_tree (wrong).
// On fixed kernel: task is NOT added (correct).

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 11, 0) && defined(ENQUEUE_DELAYED)

static struct task_struct *task_a;

static void setup(void) {
  kstep_topo_init();
  const char *smt[] = {"0", "1,2", "1,2"};
  kstep_topo_set_smt(smt, 3);
  kstep_topo_apply();

  task_a = kstep_task_create();
}

struct sched_core_cookie {
  refcount_t refcnt;
};

typedef void (*core_enqueue_fn_t)(struct rq *, struct task_struct *);
typedef void (*core_dequeue_fn_t)(struct rq *, struct task_struct *, int);

static void run(void) {
  struct rq *rq1 = cpu_rq(1);

  kstep_task_pin(task_a, 1, 1);
  kstep_tick_repeat(5);

  TRACE_INFO("task_a: pid=%d on_rq=%d cpu=%d", task_a->pid, task_a->on_rq,
             task_cpu(task_a));

  // Enable core scheduling static branch but NOT rq->core_enabled on CPUs
  // 1/2. This ensures sched_core_enqueue/dequeue can be called without
  // triggering the full core-scheduling pick path (which could crash).
  KSYM_IMPORT(__sched_core_enabled);
  static_branch_enable(KSYM___sched_core_enabled);

  KSYM_IMPORT_TYPED(atomic_t, sched_core_count);
  atomic_set(KSYM_sched_core_count, 10);

  // Give task_a a core cookie
  struct sched_core_cookie *ck = kmalloc(sizeof(*ck), GFP_KERNEL);
  if (!ck) {
    kstep_fail("kmalloc failed");
    goto cleanup;
  }
  refcount_set(&ck->refcnt, 4);
  task_a->core_cookie = (unsigned long)ck;

  // Look up sched_core_enqueue and sched_core_dequeue
  core_enqueue_fn_t core_enqueue =
      (core_enqueue_fn_t)kstep_ksym_lookup("sched_core_enqueue");
  core_dequeue_fn_t core_dequeue =
      (core_dequeue_fn_t)kstep_ksym_lookup("sched_core_dequeue");

  if (!core_enqueue || !core_dequeue) {
    kstep_fail("sched_core_enqueue/dequeue not found");
    goto cleanup;
  }

  // Manually add task_a to core_tree, then remove it (simulate the initial
  // dequeue that happens when a task enters delayed-dequeue state).
  core_enqueue(rq1, task_a);
  TRACE_INFO("After manual enqueue: enqueued=%d", sched_core_enqueued(task_a));

  core_dequeue(rq1, task_a, 0);
  TRACE_INFO("After manual dequeue: enqueued=%d", sched_core_enqueued(task_a));

  // Now simulate the delayed-dequeue state: the task's dequeue was deferred,
  // so sched_delayed is set while the task remains on the runqueue.
  task_a->se.sched_delayed = 1;
  TRACE_INFO("Set sched_delayed=1, on_rq=%d", task_a->on_rq);

  // Now call sched_core_enqueue, simulating what happens when a
  // sched_delayed task goes through enqueue_task() (e.g. during migration).
  // On BUGGY kernel: sched_core_enqueue does NOT check sched_delayed,
  //   so it adds the task to core_tree → sched_core_enqueued = 1.
  // On FIXED kernel: sched_core_enqueue sees sched_delayed=1 and returns
  //   early → sched_core_enqueued = 0.
  core_enqueue(rq1, task_a);

  bool enqueued_after = sched_core_enqueued(task_a);
  TRACE_INFO("After enqueue with sched_delayed=1: enqueued=%d",
             enqueued_after);

  // Clean up: remove from core_tree if it was added
  if (sched_core_enqueued(task_a))
    core_dequeue(rq1, task_a, 0);

  // Reset task state
  task_a->se.sched_delayed = 0;
  task_a->core_cookie = 0;

cleanup:
  // Disable core scheduling
  atomic_set(KSYM_sched_core_count, 0);
  static_branch_disable(KSYM___sched_core_enabled);

  kstep_tick_repeat(3);

  if (enqueued_after) {
    kstep_fail("sched_delayed task added to core_tree (buggy): enqueued=%d",
               enqueued_after);
  } else {
    kstep_pass("sched_delayed task correctly excluded from core_tree (fixed)");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "delayed_vs_core",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
