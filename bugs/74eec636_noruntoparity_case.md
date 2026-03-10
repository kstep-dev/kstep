# sched/fair: Fix NO_RUN_TO_PARITY case

- **Commit:** 74eec63661d46a7153d04c2e0249eeb76cc76d44
- **Affected file(s):** include/linux/sched.h, kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The EEVDF scheduler was not properly enforcing a minimum runtime quantum for entities when the RUN_TO_PARITY feature is disabled. This caused entities to be preempted prematurely instead of being allowed to run for their allocated time slice, violating the EEVDF scheduler's contract of allocating a time quantum to the selected entity before picking a new one.

## Root Cause

The `set_protect_slice()` function was only storing the deadline without considering the NO_RUN_TO_PARITY case, and `pick_eevdf()` was only checking slice protection when the RUN_TO_PARITY feature was enabled. This meant that when the feature is disabled, no minimum quantum protection was applied, causing improper preemption behavior.

## Fix Summary

Modified `set_protect_slice()` to compute a proper protection vruntime (vprot) that accounts for both the deadline and a minimum quantum (calculated from base slice) when RUN_TO_PARITY is disabled. Updated `pick_eevdf()` to always check slice protection regardless of the feature flag, and added a union field in `struct sched_entity` to properly track vprot when an entity is current on the runqueue.

## Triggering Conditions

This bug manifests when the RUN_TO_PARITY scheduler feature is disabled (default in most kernels). The vulnerable code path is in the EEVDF scheduler's task selection mechanism in `pick_eevdf()`. When RUN_TO_PARITY is disabled, entities lose slice protection and can be preempted immediately without receiving their minimum runtime quantum. The bug occurs when:
- Multiple tasks compete on a CPU with EEVDF scheduler active
- RUN_TO_PARITY feature is disabled (checked via `sched_feat(RUN_TO_PARITY)`)
- A task becomes current but gets preempted before completing its allocated time slice
- The preemption happens through `update_curr()` → `resched_next_slice()` path
- Entity loses its protection because `set_protect_slice()` only stored deadline without minimum quantum consideration
- `pick_eevdf()` was only checking slice protection when RUN_TO_PARITY was enabled

## Reproduce Strategy (kSTEP)

Create multiple competing tasks on CPU 1-2 and disable RUN_TO_PARITY to trigger premature preemption. Use `on_tick_begin` callback to monitor task switches and runtime accumulation:

```c
static void setup(void) {
  kstep_sysctl_write("kernel/sched_features", "-RUN_TO_PARITY");  // Disable feature
  task_a = kstep_task_create();
  task_b = kstep_task_create();
  kstep_task_pin(task_a, 1, 1);
  kstep_task_pin(task_b, 1, 1);
}

static void run(void) {
  kstep_task_wakeup(task_a);
  kstep_task_wakeup(task_b);
  kstep_tick_repeat(100);  // Let tasks compete for CPU time
}
```

Monitor task runtime via `sum_exec_runtime` and detect premature preemptions by checking if tasks get minimal quantum before being switched. The bug manifests as tasks getting preempted too early, violating EEVDF's runtime quantum guarantee.
