// https://github.com/torvalds/linux/commit/1560d1f6eb6b398bddd80c16676776c0325fe5fe
//
// Bug: reweight_eevdf() computes vlag = avruntime - se->vruntime without
// clamping, then the multiplication vlag * old_weight can overflow s64.
// Fix: Clamp vlag via entity_lag() before the scaling multiplication.
//
// Reproduction strategy:
//
// Under normal periodic ticks (~4ms), CFS equalises vruntimes every
// scheduling cycle, keeping vlag bounded to tens of microseconds — far
// below the ~88 seconds needed for the overflow. Running more ticks
// doesn't help; vlag simply oscillates near zero.
//
// However, on nohz_full CPUs the tick is suppressed entirely while a
// task is running alone. When the tick eventually fires (or a scheduling
// event occurs), update_curr() sees the full elapsed wall time as a
// single delta, producing an enormous vruntime jump and vlag in one shot.
// The original reporters hit this on machines with idle scheduling policy
// and nohz, where tasks ran uninterrupted for long stretches.
//
// We model this by setting step_interval_us = 10 billion (10000 seconds
// per tick). Each tick, the running group entity's vruntime jumps by
// ~100 seconds while the 9 other entities stay frozen, creating a vlag
// of ~90 seconds. A subsequent cgroup weight change triggers
// reweight_eevdf where vlag * old_weight ≈ 90e9 * 105M ≈ 9.4e18
// overflows S64_MAX (9.22e18).

#include "driver.h"
#include "internal.h"

#define NUM_GROUPS 10
static struct task_struct *tasks[NUM_GROUPS];
static char names[NUM_GROUPS][4];

static void setup(void) {
  for (int i = 0; i < NUM_GROUPS; i++) {
    snprintf(names[i], sizeof(names[i]), "g%d", i);
    kstep_cgroup_create(names[i]);
    kstep_cgroup_set_weight(names[i], 10000);
    tasks[i] = kstep_task_create();
    kstep_cgroup_add_task(names[i], tasks[i]->pid);
    kstep_task_pin(tasks[i], 1, 1);
  }
}

static int find_running(void) {
  for (int i = 0; i < NUM_GROUPS; i++)
    if (tasks[i]->on_cpu)
      return i;
  return -1;
}

static void run(void) {
  for (int i = 0; i < NUM_GROUPS; i++)
    kstep_task_wakeup(tasks[i]);

  // Each tick simulates 10000 seconds. After 20 ticks the scheduler
  // has cycled through all entities, roughly equalising vruntimes.
  kstep_tick_repeat(20);

  // One more tick: the running entity's vruntime jumps by ~100s.
  int ran = find_running();
  int ref = (ran + 1) % NUM_GROUPS;
  kstep_tick();

  struct sched_entity *ran_se = tasks[ran]->se.parent;
  struct sched_entity *ref_se = tasks[ref]->se.parent;

  TRACE_INFO("Before reweight: g%d vrt=%llu g%d vrt=%llu",
             ran, ran_se->vruntime, ref, ref_se->vruntime);

  // Reweight the group that just ran. Its vlag ≈ -90s, weight ≈ 105M.
  // vlag * old_weight ≈ 9.4e18 > S64_MAX on the buggy kernel.
  kstep_cgroup_set_weight(names[ran], 1);

  // Compare to a reference entity that was NOT reweighted. After 20+
  // equalising ticks, all entities' vruntimes are within ~200s of each
  // other. The reweight should keep it in that range (fixed kernel) or
  // blow it out by hundreds of thousands of seconds (buggy kernel).
  s64 drift = (s64)(ran_se->vruntime - ref_se->vruntime);
  TRACE_INFO("After reweight: g%d vrt=%llu drift=%lld s from g%d",
             ran, ran_se->vruntime, drift / 1000000000LL, ref);

  s64 abs_drift = drift < 0 ? -drift : drift;
  if (abs_drift > 50000000000000LL) // > 50000 seconds
    kstep_fail("vlag overflow: g%d drifted %lld s from g%d",
               ran, drift / 1000000000LL, ref);
  else
    kstep_pass("g%d within %lld s of g%d (no overflow)",
               ran, drift / 1000000000LL, ref);
}

KSTEP_DRIVER_DEFINE{
    .name = "vlag_overflow",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000000000ULL,
    .print_rq = false,
};
