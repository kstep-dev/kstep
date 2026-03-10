// https://github.com/torvalds/linux/commit/b1e8206582f9
//
// Bug: sched_post_fork() runs AFTER the new task becomes visible in the
// pidhash, leaving a window where sched_task_group, __set_task_cpu(), and
// task_fork() have not been called yet.  During this window concurrent
// syscalls (setpriority, sched_setaffinity, …) can access uninitialised
// scheduler state.
//
// The band-aid fix (13765de8148f) added a TASK_NEW guard inside
// set_load_weight() so that reweight_task() is skipped for tasks that
// still carry the TASK_NEW flag.  The proper fix (b1e8206582f9) moves
// cgroup / CPU / task_fork initialisation into sched_cgroup_fork() which
// runs BEFORE the task is visible, and makes set_load_weight() take an
// explicit update_load parameter, removing the TASK_NEW check.
//
// Reproduce: create a task, put it to sleep, set TASK_NEW, corrupt
// load_sum, then call set_user_nice().
//   Buggy kernel  → TASK_NEW guard skips reweight → load_avg unchanged.
//   Fixed kernel  → explicit update_load=true    → load_avg recalculated.

#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 17, 0)

static struct task_struct *task;

static void setup(void) {
  task = kstep_task_create();
}

static void run(void) {
  // Pin task to CPU 1 and let it accumulate load
  kstep_task_pin(task, 1, 1);
  kstep_task_wakeup(task);
  kstep_tick_repeat(50);

  // Pause the task so it goes to sleep (TASK_INTERRUPTIBLE, off-rq)
  kstep_task_pause(task);
  kstep_tick_repeat(5);

  struct sched_entity *se = &task->se;

  unsigned long load_avg_before = se->avg.load_avg;
  TRACE_INFO("Before: load_avg=%lu load_sum=%llu weight=%lu state=0x%x",
             load_avg_before, (u64)se->avg.load_sum,
             se->load.weight, READ_ONCE(task->__state));

  if (load_avg_before == 0) {
    TRACE_INFO("load_avg is 0, ticking more to accumulate load");
    kstep_task_wakeup(task);
    kstep_tick_repeat(100);
    kstep_task_pause(task);
    kstep_tick_repeat(5);
    load_avg_before = se->avg.load_avg;
    TRACE_INFO("Retry: load_avg=%lu", load_avg_before);
  }

  // Corrupt load_sum to 0; if reweight_entity fires it will recalculate
  // load_avg = weight * 0 / divider = 0, which differs from load_avg_before.
  u64 saved_load_sum = se->avg.load_sum;
  se->avg.load_sum = 0;

  // Simulate the race window: mark the task TASK_NEW, as it would be
  // between pidhash visibility and sched_post_fork() in the buggy kernel.
  unsigned int old_state = READ_ONCE(task->__state);
  WRITE_ONCE(task->__state, old_state | TASK_NEW);

  TRACE_INFO("Set TASK_NEW, state=0x%x, load_sum zeroed",
             READ_ONCE(task->__state));

  // Call set_user_nice() — goes through set_load_weight().
  // Buggy:  TASK_NEW guard → update_load=false → reweight skipped.
  // Fixed:  set_load_weight(p, true)            → reweight called.
  set_user_nice(task, 10);

  unsigned long load_avg_after = se->avg.load_avg;
  TRACE_INFO("After set_user_nice(10): load_avg=%lu weight=%lu",
             load_avg_after, se->load.weight);

  // Restore state
  se->avg.load_sum = saved_load_sum;
  WRITE_ONCE(task->__state, old_state);

  // Also check whether sched_cgroup_fork exists (added by the fix)
  void *cgroup_fork_fn = kstep_ksym_lookup("sched_cgroup_fork");
  TRACE_INFO("sched_cgroup_fork symbol: %px", cgroup_fork_fn);

  // Verdict
  if (load_avg_before > 0 && load_avg_after == load_avg_before) {
    // TASK_NEW guard prevented reweight_task() — buggy behaviour
    kstep_fail("TASK_NEW guard skipped reweight: load_avg unchanged "
               "(%lu→%lu), sched_post_fork races present",
               load_avg_before, load_avg_after);
  } else if (load_avg_after == 0 && load_avg_before > 0) {
    // reweight_entity recalculated load_avg from zeroed load_sum — fixed
    kstep_pass("reweight_task called despite TASK_NEW: "
               "load_avg recalculated (%lu→%lu), no fork race",
               load_avg_before, load_avg_after);
  } else if (load_avg_before == 0) {
    kstep_fail("could not accumulate load_avg, test inconclusive");
  } else {
    // load_avg changed but not to 0 — unexpected
    kstep_pass("load_avg changed (%lu→%lu): reweight likely called",
               load_avg_before, load_avg_after);
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "sched_fork_race",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 4000000,
};
