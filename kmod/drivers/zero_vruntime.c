// https://github.com/torvalds/linux/commit/b3d99f43c72b56cf7a104a364e7fb34b0702828b
//
// Bug: zero_vruntime is only updated via __enqueue_entity() and
// __dequeue_entity(). When a single task runs on a CPU, pick_next_task()
// always returns that task via put_prev_set_next_task(), which never calls
// __enqueue_entity() or __dequeue_entity(). Thus zero_vruntime stagnates
// while vruntime advances, causing entity_key() to grow without bound.
// This leads to overflow in key*weight products used by sum_w_vruntime
// and corrupted scheduling decisions when new tasks arrive.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(7, 0, 0)

static struct task_struct *solo_task;
static struct task_struct *new_task;

static void setup(void) {
  solo_task = kstep_task_create();
  new_task = kstep_task_create();
}

static void on_tick(void) {
  struct cfs_rq *cfs_rq = &cpu_rq(1)->cfs;
  struct sched_entity *curr = cfs_rq->curr;

  if (!curr)
    return;

  s64 key = (s64)(curr->vruntime - cfs_rq->zero_vruntime);

  struct kstep_json json;
  kstep_json_begin(&json);
  kstep_json_field_str(&json, "event", "tick");
  kstep_json_field_u64(&json, "vruntime", curr->vruntime);
  kstep_json_field_u64(&json, "zero_vruntime", cfs_rq->zero_vruntime);
  kstep_json_field_s64(&json, "entity_key", key);
  kstep_json_field_s64(&json, "sum_w_vruntime", cfs_rq->sum_w_vruntime);
  kstep_json_field_u64(&json, "sum_weight", cfs_rq->sum_weight);
  kstep_json_field_u64(&json, "nr_running", cpu_rq(1)->nr_running);
  kstep_json_end(&json);
}

static void run(void) {
  struct cfs_rq *cfs_rq = &cpu_rq(1)->cfs;

  // Phase 1: Run a single task alone on CPU 1 for many ticks.
  // zero_vruntime should track vruntime, but won't on the buggy kernel.
  kstep_task_kernel_wakeup(solo_task);
  kstep_tick_repeat(5);

  s64 key_before = (s64)(solo_task->se.vruntime - cfs_rq->zero_vruntime);
  TRACE_INFO("Phase 1 start: key=%lld zero_vruntime=%llu vruntime=%llu",
             key_before, cfs_rq->zero_vruntime, solo_task->se.vruntime);

  // Run solo for a long time - zero_vruntime should stay close to vruntime
  // but on buggy kernel, zero_vruntime stagnates.
  kstep_tick_repeat(200);

  s64 key_after = (s64)(solo_task->se.vruntime - cfs_rq->zero_vruntime);
  TRACE_INFO("Phase 1 end: key=%lld zero_vruntime=%llu vruntime=%llu",
             key_after, cfs_rq->zero_vruntime, solo_task->se.vruntime);

  // Phase 2: Wake a second task. On buggy kernel, entity_key() is huge,
  // so the key*weight product in sum_w_vruntime will be way off.
  kstep_task_kernel_wakeup(new_task);
  kstep_tick();

  s64 key_new = (s64)(new_task->se.vruntime - cfs_rq->zero_vruntime);
  s64 key_solo = (s64)(solo_task->se.vruntime - cfs_rq->zero_vruntime);
  TRACE_INFO("Phase 2: new_task key=%lld solo_task key=%lld", key_new,
             key_solo);
  TRACE_INFO("Phase 2: sum_w_vruntime=%lld sum_weight=%llu",
             cfs_rq->sum_w_vruntime, (u64)cfs_rq->sum_weight);

  // The bug: on buggy kernel, key grows unbounded with tick count.
  // On fixed kernel, key stays near zero (bounded by lag).
  // Use 50ms worth of vruntime as threshold - anything larger means
  // zero_vruntime is not tracking properly.
  s64 threshold = 50000000LL; // 50ms in ns
  if (key_after > threshold || key_after < -threshold) {
    kstep_fail("entity_key drifted to %lld (zero_vruntime stale)", key_after);
  } else {
    kstep_pass("entity_key bounded at %lld (zero_vruntime tracking)",
               key_after);
  }

  kstep_tick_repeat(10);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
static void on_tick(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "zero_vruntime",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick,
    .step_interval_us = 10000,
};
