// https://github.com/torvalds/linux/commit/87ca4f9efbd7
//
// Bug: dup_user_cpus_ptr() does not clear dst->user_cpus_ptr before checking
// src->user_cpus_ptr. When src->user_cpus_ptr is concurrently freed and
// NULLed by do_set_cpus_allowed(), the function returns 0 without modifying
// dst->user_cpus_ptr, leaving it with the stale pointer inherited from
// dup_task_struct(). This causes use-after-free / double-free on task exit.
//
// Fix: Clear dst->user_cpus_ptr = NULL upfront, then check src under pi_lock.
//
// Reproduce: Simulate the post-race state (src->user_cpus_ptr = NULL,
// dst->user_cpus_ptr = sentinel) and call dup_user_cpus_ptr(). On the buggy
// kernel, dst retains the stale sentinel. On the fixed kernel, dst is cleared.

#include <linux/slab.h>
#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 2, 0)

static struct task_struct *src_task;
static struct task_struct *dst_task;

static void setup(void) {
  src_task = kstep_task_create();
  dst_task = kstep_task_create();
}

static void run(void) {
  typedef int (*dup_fn_t)(struct task_struct *, struct task_struct *, int);

  dup_fn_t dup_fn = (dup_fn_t)kstep_ksym_lookup("dup_user_cpus_ptr");
  if (!dup_fn) {
    kstep_fail("cannot resolve dup_user_cpus_ptr");
    return;
  }

  // Pin src_task to allocate user_cpus_ptr via sched_setaffinity
  kstep_task_pin(src_task, 1, 1);
  kstep_task_wakeup(src_task);
  kstep_tick_repeat(5);

  if (!src_task->user_cpus_ptr) {
    kstep_fail("src has no user_cpus_ptr after pin");
    return;
  }

  TRACE_INFO("src user_cpus_ptr=%px mask=%*pbl", src_task->user_cpus_ptr,
             cpumask_pr_args(src_task->user_cpus_ptr));

  // Allocate a sentinel cpumask to simulate dup_task_struct's memcpy
  struct cpumask *sentinel = kmalloc(cpumask_size(), GFP_KERNEL);
  if (!sentinel) {
    kstep_fail("sentinel allocation failed");
    return;
  }
  cpumask_setall(sentinel);

  // Save original pointers for restoration
  struct cpumask *orig_src = src_task->user_cpus_ptr;
  struct cpumask *orig_dst = dst_task->user_cpus_ptr;

  // Simulate the race:
  //  - dup_task_struct copies src->user_cpus_ptr into dst
  //  - do_set_cpus_allowed frees and NULLs src->user_cpus_ptr
  dst_task->user_cpus_ptr = sentinel;
  src_task->user_cpus_ptr = NULL;

  TRACE_INFO("Race state: src->user_cpus_ptr=NULL, dst->user_cpus_ptr=%px",
             dst_task->user_cpus_ptr);

  // Call dup_user_cpus_ptr(dst, src, 0)
  // Buggy: sees !src->user_cpus_ptr, returns 0 without touching dst
  // Fixed: clears dst->user_cpus_ptr=NULL first, then returns 0
  int ret = dup_fn(dst_task, src_task, 0);

  struct cpumask *result = dst_task->user_cpus_ptr;
  TRACE_INFO("After dup: ret=%d dst->user_cpus_ptr=%px", ret, result);

  // Restore original state to avoid corruption
  src_task->user_cpus_ptr = orig_src;
  dst_task->user_cpus_ptr = orig_dst;
  kfree(sentinel);

  if (result == sentinel) {
    kstep_fail("dst->user_cpus_ptr not cleared (stale ptr %px retained): "
               "use-after-free in forked task",
               sentinel);
  } else if (result == NULL) {
    kstep_pass("dst->user_cpus_ptr correctly cleared to NULL");
  } else {
    kstep_fail("unexpected dst->user_cpus_ptr=%px", result);
  }

  kstep_tick_repeat(3);
}

KSTEP_DRIVER_DEFINE{
    .name = "dup_user_cpus_uaf",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#else
static void run(void) { TRACE_INFO("Skipped: wrong kernel version"); }
KSTEP_DRIVER_DEFINE{.name = "dup_user_cpus_uaf", .run = run};
#endif
