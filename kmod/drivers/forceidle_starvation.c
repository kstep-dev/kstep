// https://github.com/torvalds/linux/commit/8039e96fcc1d
//
// Bug: When core scheduling is enabled and a sibling CPU is forced idle,
// a single long-running task on the other sibling can starve the forced-idle
// sibling indefinitely. No tick-based mechanism exists to detect and rescue
// the starved sibling.
//
// Fix: Adds task_tick_core() to task_tick_fair() that checks
// rq->core->core_forceidle && rq->cfs.nr_running == 1 && slice_used
// and calls resched_curr() to give the forced-idle sibling a chance.
// Also moves core_forceidle from per-rq to core-level shared state.
//
// Test: Enable core scheduling on an SMT pair (CPU 1 + CPU 2). Pin a single
// long-running CFS task to CPU 1. Set core_forceidle to simulate a forced-idle
// sibling. Run ticks and check if TIF_NEED_RESCHED is set on the running task.
// Buggy: no resched (starvation). Fixed: resched triggered by task_tick_core.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 13, 0) && defined(CONFIG_SCHED_CORE)

static struct task_struct *task;
static int resched_seen;
static int tick_count;

static void on_tick_end(void) {
  struct rq *rq1 = cpu_rq(1);

  if (!rq1->curr || rq1->curr->pid == 0)
    return;

  tick_count++;

  TRACE_INFO("tick_end %d: CPU1 curr=%s pid=%d need_resched=%d nr_running=%d "
             "exec=%llu prev_exec=%llu forceidle=%d",
             tick_count, rq1->curr->comm, rq1->curr->pid,
             test_tsk_need_resched(rq1->curr), rq1->cfs.nr_running,
             rq1->curr->se.sum_exec_runtime,
             rq1->curr->se.prev_sum_exec_runtime,
             rq1->core->core_forceidle);

  // Detect reschedule: prev_sum_exec_runtime changes when task is re-picked
  if (tick_count > 3 && rq1->curr->se.prev_sum_exec_runtime > 0)
    resched_seen = 1;

  // Re-set core_forceidle (pick_next_task clears it on reschedule)
  if (rq1->core_enabled)
    rq1->core->core_forceidle = 1;
}

static void setup(void) {
  kstep_topo_init();
  const char *smt[] = {"0", "1,2", "1,2"};
  kstep_topo_set_smt(smt, 3);
  kstep_topo_apply();

  task = kstep_task_create();
}

static void run(void) {
  struct rq *rq1 = cpu_rq(1);
  struct rq *rq2 = cpu_rq(2);

  kstep_task_pin(task, 1, 1);
  kstep_tick_repeat(3);

  TRACE_INFO("Initial: CPU1 curr=%s pid=%d nr_running=%d",
             rq1->curr->comm, rq1->curr->pid, rq1->cfs.nr_running);

  // Enable core scheduling
  KSYM_IMPORT(__sched_core_enabled);
  static_branch_enable(KSYM___sched_core_enabled);

  // kSTEP is single-threaded with controlled ticks, direct field writes are safe
  rq1->core_enabled = true;
  rq2->core_enabled = true;

  TRACE_INFO("Core scheduling enabled, core=%px, rq1=%px, rq2=%px",
             rq1->core, rq1, rq2);

  // Set core_forceidle to simulate forced-idle sibling.
  rq1->core->core_forceidle = 1;

  TRACE_INFO("core_forceidle set on core rq, CPU1 nr_running=%d",
             rq1->cfs.nr_running);

  // Clear any existing resched flag so we only detect new reschedules
  if (test_tsk_need_resched(rq1->curr))
    clear_tsk_need_resched(rq1->curr);

  // Run many ticks to let the task consume its slice.
  // On the fixed kernel, task_tick_core will detect the starvation
  // and call resched_curr. On the buggy kernel, no such check exists.
  resched_seen = 0;
  tick_count = 0;
  kstep_tick_repeat(30);

  TRACE_INFO("After 30 ticks: resched_seen=%d", resched_seen);

  // Clean up
  rq1->core->core_forceidle = 0;
  rq1->core_enabled = false;
  rq2->core_enabled = false;

  static_branch_disable(KSYM___sched_core_enabled);

  kstep_tick_repeat(3);

  if (resched_seen) {
    kstep_pass("task_tick_core detected forced idle starvation and "
               "triggered resched (fixed)");
  } else {
    kstep_fail("forced idle sibling starvation: task ran for 30 ticks "
               "with core_forceidle set but no resched triggered (buggy)");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
static void on_tick_end(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "forceidle_starvation",
    .setup = setup,
    .run = run,
    .on_tick_end = on_tick_end,
    .step_interval_us = 10000,
    .tick_interval_ns = 4000000,
};
