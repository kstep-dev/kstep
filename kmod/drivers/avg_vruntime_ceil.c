// https://github.com/torvalds/linux/commit/650cad561cce04b62a8c8e0446b685ef171bc3bb
//
// Bug: avg_vruntime() uses integer division which truncates toward zero.
// For negative avg values, this acts as ceiling instead of floor, causing
// the returned position to be ineligible.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 18, 0)

static struct task_struct *task_a;
static struct task_struct *task_b;
static struct task_struct *task_c;

static void setup(void) {
  task_a = kstep_task_create();
  task_b = kstep_task_create();
  task_c = kstep_task_create();
}

static void run(void) {
  struct cfs_rq *cfs_rq = &cpu_rq(1)->cfs;

  // A: nice 1 (weight 820), B: nice 0 (weight 1024).
  // Different weights ensure avg/load is not evenly divisible.
  kstep_task_set_prio(task_a, 1);

  kstep_task_wakeup(task_a);
  kstep_task_wakeup(task_b);

  // Run so both tasks accumulate vruntime.
  // B (higher weight) gets more CPU but its vruntime grows slower,
  // so B ends up with lower vruntime -> positive lag when paused.
  kstep_tick_repeat(10);

  // Tick until A is curr (B is on the tree with positive lag)
  for (int i = 0; i < 20; i++) {
    if (cfs_rq->curr == &task_a->se)
      break;
    kstep_tick();
  }

  // Pause B. The scheduler saves B's vlag via update_entity_lag().
  kstep_task_pause(task_b);

  // Tick so B processes the PAUSE signal and gets dequeued
  kstep_tick_repeat(2);

  TRACE_INFO("B vlag=%lld after pause", task_b->se.vlag);

  // A runs alone. min_vruntime advances with A's vruntime.
  kstep_tick_repeat(10);

  // Wake B. place_entity() uses vlag to set B's vruntime.
  // With positive vlag, B is placed below min_vruntime (negative key).
  kstep_task_wakeup(task_b);

  s64 key_b = (s64)(task_b->se.vruntime - cfs_rq->min_vruntime);
  TRACE_INFO("B: vruntime=%llu key=%lld min_vruntime=%llu",
             task_b->se.vruntime, key_b, cfs_rq->min_vruntime);

  // Compute the full avg value inside avg_vruntime()
  struct sched_entity *curr = cfs_rq->curr;
  s64 avg = cfs_rq->avg_vruntime;
  long load = cfs_rq->avg_load;
  if (curr && curr->on_rq) {
    unsigned long weight = scale_load_down(curr->load.weight);
    s64 key = (s64)(curr->vruntime - cfs_rq->min_vruntime);
    avg += key * weight;
    load += weight;
  }

  KSYM_IMPORT(avg_vruntime);
  u64 V = KSYM_avg_vruntime(cfs_rq);
  s64 key_v = (s64)(V - cfs_rq->min_vruntime);

  TRACE_INFO("State: avg=%lld load=%ld V=%llu key_v=%lld", avg, load, V, key_v);

  if (avg >= 0 || load <= 0) {
    kstep_fail("avg=%lld not negative (vlag may have been non-positive)", avg);
    return;
  }

  // Direct invariant: avg >= key_v * load must hold
  int invariant = (avg >= key_v * load);
  TRACE_INFO("Invariant avg >= key_v*load: %lld >= %lld -> %s", avg,
             key_v * load, invariant ? "PASS" : "FAIL (BUG)");

  // Wake C (vlag=0) -> placed at avg_vruntime() by place_entity()
  kstep_task_wakeup(task_c);

  s64 key_c = (s64)(task_c->se.vruntime - cfs_rq->min_vruntime);
  int c_eligible = kstep_eligible(&task_c->se);

  TRACE_INFO("C: vruntime=%llu key=%lld eligible=%d", task_c->se.vruntime,
             key_c, c_eligible);

  if (!c_eligible) {
    kstep_fail("task at avg_vruntime() not eligible: "
               "avg=%lld load=%ld key_c=%lld",
               avg, load, key_c);
  } else {
    kstep_pass("task at avg_vruntime() is eligible");
  }

  kstep_tick_repeat(5);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "avg_vruntime_ceil",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
