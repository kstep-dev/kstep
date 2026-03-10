// https://github.com/torvalds/linux/commit/76f970ce51c8
//
// Bug: An optimization in sched_move_task() added an early bailout when
// p->sched_task_group == sched_get_task_group(p), skipping the full
// dequeue/sched_change_group/enqueue sequence. For tasks in a non-root
// cgroup (even with autogroup enabled), autogroup_task_group returns
// the cgroup task_group because task_wants_autogroup returns false for
// non-root cgroups. This means the early bail always triggers for tasks
// in non-root cgroups during sched_autogroup_exit_task(), preventing
// detach_task_cfs_rq() from being called. The result is stale group
// utilization causing incorrect load balancing decisions.
//
// Fix: Revert the optimization so sched_move_task() always performs the
// full dequeue/sched_change_group/enqueue.
//
// Observable: On the buggy kernel, sched_move_task() returns early
// before calling update_rq_clock(), so the rq clock is not advanced.
// On the fixed kernel, update_rq_clock() is always called.

#include <linux/version.h>

#include "driver.h"
#include "internal.h"
#include "../user.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)

#include <linux/sched/signal.h>

KSYM_IMPORT_TYPED(unsigned int, sysctl_sched_autogroup_enabled);

static struct task_struct *parent;

static void setup(void) {
  // Disable autogroup sysctl so autogroup_task_group always returns the
  // cgroup tg. This ensures sched_get_task_group(p) == p->sched_task_group
  // for tasks in non-root cgroups, triggering the buggy early bail.
  *KSYM_sysctl_sched_autogroup_enabled = 0;

  kstep_topo_init();
  kstep_topo_apply();

  // Create a non-root cgroup. Tasks in non-root cgroups have
  // task_wants_autogroup return false, so autogroup_task_group
  // returns the cgroup tg instead of the autogroup tg.
  kstep_cgroup_create("spawn");

  parent = kstep_task_create();
  kstep_cgroup_add_task("spawn", parent->pid);
}

typedef void (*sched_move_task_fn_t)(struct task_struct *, bool);

static void run(void) {
  kstep_task_pin(parent, 1, 1);
  kstep_tick_repeat(10);

  sched_move_task_fn_t fn =
      (sched_move_task_fn_t)kstep_ksym_lookup("sched_move_task");
  if (!fn) {
    kstep_fail("could not find sched_move_task symbol");
    return;
  }

  // Read rq clock before calling sched_move_task.
  // On the buggy kernel, the early bail returns before update_rq_clock(),
  // so the clock won't advance. On the fixed kernel, update_rq_clock()
  // is always called, advancing the clock.
  struct rq *rq = cpu_rq(1);

  // Verify parent is running on CPU 1
  bool is_running = (rq->donor == parent);
  bool is_queued = task_on_rq_queued(parent);
  TRACE_INFO("parent: running=%d queued=%d cpu=%d",
             is_running, is_queued, task_cpu(parent));

  if (!is_running) {
    kstep_fail("parent not running on CPU 1, cannot test");
    return;
  }

  // Clear any existing need_resched by doing a tick
  kstep_tick();

  // Check TIF_NEED_RESCHED before sched_move_task
  int resched_before = test_tsk_need_resched(parent);

  // Call sched_move_task as sched_autogroup_exit_task would.
  // With sysctl=0 and task in non-root cgroup "spawn":
  //   sched_get_task_group(p) = "spawn" cgroup tg
  //   p->sched_task_group    = "spawn" cgroup tg
  //   Buggy: match → early return (no dequeue/enqueue, no resched_curr)
  //   Fixed: no early bail → dequeue/put_prev/change_group/enqueue/
  //          set_next/resched_curr → TIF_NEED_RESCHED set
  fn(parent, true);

  int resched_after = test_tsk_need_resched(parent);

  TRACE_INFO("TIF_NEED_RESCHED: before=%d after=%d",
             resched_before, resched_after);

  if (!resched_after) {
    kstep_fail("sched_move_task bailed early: TIF_NEED_RESCHED not set "
               "(detach_task_cfs_rq skipped)");
  } else {
    kstep_pass("sched_move_task full path: TIF_NEED_RESCHED set "
               "(resched_curr called after dequeue/enqueue)");
  }

  kstep_tick_repeat(5);
}

#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

static void on_tick_begin(void) {
  kstep_json_print_2kv("type", "util_avg", "val", "%llu",
                        cpu_rq(1)->cfs.avg.util_avg);
}

KSTEP_DRIVER_DEFINE{
    .name = "sched_move",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};
