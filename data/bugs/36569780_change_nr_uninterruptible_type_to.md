# sched: Change nr_uninterruptible type to unsigned long

- **Commit:** 36569780b0d64de283f9d6c2195fd1a43e221ee8
- **Affected file(s):** kernel/sched/loadavg.c, kernel/sched/sched.h
- **Subsystem:** Core scheduler (loadavg)

## Bug Description

When large numbers of tasks are migrated off a CPU after entering an uninterruptible state, the per-CPU `nr_uninterruptible` counter can grow beyond INT_MAX. Since it was defined as `unsigned int`, this causes integer overflow, resulting in wraparound and incorrect load average calculations. The load average is computed by summing `nr_uninterruptible` across all CPUs, so overflow on even one CPU corrupts the final result.

## Root Cause

A prior commit (e6fe3f422be1) reduced `nr_uninterruptible` from `unsigned long` to `unsigned int` as part of a 32-bit optimization. However, the counter's semantics allow it to grow very large over time as tasks transition through uninterruptible states and migrate between CPUs. The `unsigned int` type is too narrow to hold these values, leading to overflow.

## Fix Summary

The fix reverts `nr_uninterruptible` back to `unsigned long` in both the struct definition (kernel/sched/sched.h) and the load calculation code (kernel/sched/loadavg.c, where the cast is changed from `(int)` to `(long)`). This prevents overflow and ensures correct load average computation.

## Triggering Conditions

The bug requires accumulating a large number of uninterruptible tasks on a single CPU that subsequently migrate away. Specifically:
- A CPU must have >2^32 tasks that entered uninterruptible sleep state and then migrated to other CPUs
- Each task migration leaves the source CPU's `nr_uninterruptible` counter incremented  
- The counter accumulates over time since only the global sum across all CPUs is meaningful
- When `nr_uninterruptible` exceeds UINT_MAX (4,294,967,295), it wraps around due to unsigned int overflow
- The wraparound causes `calc_load_fold_active()` to compute incorrect load average values
- This affects system load reporting and potentially scheduler decisions based on load metrics

## Reproduce Strategy (kSTEP)

Requires multiple CPUs (at least 3: CPU 0 reserved, plus CPUs 1-2 for testing):
- **Setup**: Create ~5000+ tasks using `kstep_task_create()`, pin them to CPU 1 initially with `kstep_task_pin()`
- **Trigger uninterruptible state**: Use `kstep_task_pause()` to put tasks into uninterruptible sleep on CPU 1
- **Force migration**: After pausing, repin tasks to CPU 2 with `kstep_task_pin(tasks[i], 2, 2)` and wake with `kstep_task_wakeup()`
- **Repeat**: Cycle thousands of iterations of pause-on-CPU1 + migrate-to-CPU2 pattern to accumulate `nr_uninterruptible`
- **Monitor**: Use `on_tick_begin()` callback to log `cpu_rq(1)->nr_uninterruptible` and detect when it wraps around
- **Verify bug**: Check if load average calculations in `avenrun[]` become incorrect due to the overflow
- **Detection**: Compare expected vs actual load average or observe `nr_uninterruptible` suddenly dropping from high values
