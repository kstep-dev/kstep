// https://github.com/torvalds/linux/commit/91caa5ae2424
//
// Bug: sched_core_update_cookie() uses a captured `enqueued` boolean from
// before the cookie change. If a task was on the runqueue but had no cookie
// (i.e., not in the core tree), assigning a cookie will NOT enqueue it into
// the core tree. This causes unnecessary force idle on SMT siblings.
//
// Fix: Change re-enqueue condition from `if (enqueued)` to
// `if (cookie && task_on_rq_queued(p))`.
//
// Test: Enable core scheduling on an SMT pair. Run uncookied tasks, then
// assign a cookie via __sched_core_set() which inlines the buggy/fixed
// sched_core_update_cookie(). On the buggy kernel tasks are NOT enqueued
// in the core tree. On the fixed kernel they ARE.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 19, 0)

static struct task_struct *task_a, *task_b;

static void setup(void) {
  kstep_topo_init();
  const char *smt[] = {"0", "1,2", "1,2"};
  kstep_topo_set_smt(smt, 3);
  kstep_topo_apply();

  task_a = kstep_task_create();
  task_b = kstep_task_create();
}

struct sched_core_cookie {
  refcount_t refcnt;
};

typedef void (*core_set_fn_t)(struct task_struct *, unsigned long);
typedef void (*core_dequeue_fn_t)(struct rq *, struct task_struct *, int);

static void run(void) {
  struct rq *rq0 = cpu_rq(0);
  struct rq *rq1 = cpu_rq(1);
  struct rq *rq2 = cpu_rq(2);

  kstep_task_pin(task_a, 1, 1);
  kstep_task_pin(task_b, 2, 2);
  kstep_tick_repeat(5);

  TRACE_INFO("task_a: pid=%d on_rq=%d cpu=%d", task_a->pid, task_a->on_rq,
             task_cpu(task_a));
  TRACE_INFO("task_b: pid=%d on_rq=%d cpu=%d", task_b->pid, task_b->on_rq,
             task_cpu(task_b));

  // Enable core scheduling state on rqs
  KSYM_IMPORT(__sched_core_enabled);
  static_branch_enable(KSYM___sched_core_enabled);
  rq0->core_enabled = true;
  rq1->core_enabled = true;
  rq2->core_enabled = true;

  // Pre-set sched_core_count to prevent sched_core_put from triggering
  // __sched_core_disable when we clean up
  KSYM_IMPORT_TYPED(atomic_t, sched_core_count);
  atomic_set(KSYM_sched_core_count, 10);

  TRACE_INFO("Before: task_a cookie=%lu enqueued=%d on_rq=%d",
             task_a->core_cookie, sched_core_enqueued(task_a),
             task_a->on_rq);
  TRACE_INFO("Before: task_b cookie=%lu enqueued=%d on_rq=%d",
             task_b->core_cookie, sched_core_enqueued(task_b),
             task_b->on_rq);

  // Look up __sched_core_set (non-inlined, calls sched_core_update_cookie)
  core_set_fn_t core_set =
      (core_set_fn_t)kstep_ksym_lookup("__sched_core_set");

  if (!core_set) {
    kstep_fail("__sched_core_set not found");
    goto cleanup;
  }
  TRACE_INFO("Found __sched_core_set at %px", core_set);

  // Allocate a real cookie struct (refcount=4: 2 tasks + margin)
  struct sched_core_cookie *ck = kmalloc(sizeof(*ck), GFP_KERNEL);
  if (!ck) {
    kstep_fail("kmalloc failed");
    goto cleanup;
  }
  refcount_set(&ck->refcnt, 4);
  unsigned long cookie = (unsigned long)ck;

  // Call __sched_core_set which internally calls sched_core_update_cookie.
  // On the BUGGY kernel, update_cookie will NOT enqueue the task into
  // the core tree because `enqueued` was false (task had no prior cookie).
  // On the FIXED kernel, it checks `cookie && task_on_rq_queued(p)` instead.
  core_set(task_a, cookie);
  core_set(task_b, cookie);

  // Check if tasks were enqueued into the core tree
  bool a_enqueued = sched_core_enqueued(task_a);
  bool b_enqueued = sched_core_enqueued(task_b);
  TRACE_INFO("After: task_a cookie=%lu enqueued=%d on_rq=%d",
             task_a->core_cookie, a_enqueued, task_a->on_rq);
  TRACE_INFO("After: task_b cookie=%lu enqueued=%d on_rq=%d",
             task_b->core_cookie, b_enqueued, task_b->on_rq);

  // Clean up: manually clear cookies and dequeue from core tree.
  // Avoid calling __sched_core_set(task, 0) which triggers resched_curr
  // and can cause crashes during cleanup.
  core_dequeue_fn_t core_dequeue =
      (core_dequeue_fn_t)kstep_ksym_lookup("sched_core_dequeue");
  if (core_dequeue) {
    if (sched_core_enqueued(task_a))
      core_dequeue(rq1, task_a, 0);
    if (sched_core_enqueued(task_b))
      core_dequeue(rq2, task_b, 0);
  }
  task_a->core_cookie = 0;
  task_b->core_cookie = 0;

cleanup:
  rq0->core_enabled = false;
  rq1->core_enabled = false;
  rq2->core_enabled = false;
  atomic_set(KSYM_sched_core_count, 0);
  static_branch_disable(KSYM___sched_core_enabled);
  kstep_tick_repeat(3);

  if (!a_enqueued || !b_enqueued) {
    kstep_fail("task not enqueued into core tree after cookie update: "
               "a=%d b=%d (buggy)", a_enqueued, b_enqueued);
  } else {
    kstep_pass("tasks properly enqueued into core tree (fixed)");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "core_enqueue",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
