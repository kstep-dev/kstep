# sched: psi: fix bogus pressure spikes from aggregation race

- **Commit:** 3840cbe24cf060ea05a585ca497814609f5d47d1
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

A race condition between reader aggregation and state changes causes sporadic, non-sensical spikes in cumulative pressure time (reported when reading cpu.pressure at a high rate). When an aggregator snoops a live stall that hasn't concluded yet, the stall duration is calculated based on the current time. However, if the state change concludes with a timestamp before the aggregator's snapshot, the recorded stall time becomes less than the aggregator's last snapshot, causing an underflow and a bogus delta value that results in erratic jumps in pressure measurements.

## Root Cause

The clock read for concluding a state change was not synchronized against aggregators—it occurred outside the seqlock protection. This allowed aggregators to race and snoop a stall with a longer duration than what would actually be recorded when the state concludes, leading to negative deltas when comparing against previously recorded snapshots.

## Fix Summary

The fix moves the `cpu_clock()` read into the seqlock-protected critical section (`write_seqcount_begin/end`). This ensures that aggregators cannot snoop live stalls past the timestamp that is recorded when the state concludes, eliminating the race condition and preventing underflow-induced pressure spikes.

## Triggering Conditions

The bug occurs in the PSI subsystem during concurrent execution of `psi_group_change()` and PSI pressure aggregation. It requires frequent task state transitions (sleep/wake cycles) to generate active stalls while PSI readers sample pressure metrics at high frequency. The race window exists between the aggregator's `cpu_clock()` read (when snooping live stalls) and the state change concluder's `cpu_clock()` read (outside seqlock protection). CPU pressure triggers most reliably due to high scheduling event frequency. The bug manifests as sporadic underflow in delta calculations when the aggregator observes a stall duration longer than what gets recorded.

## Reproduce Strategy (kSTEP)

Use at least 3 CPUs (CPU 0 reserved). In `setup()`, create 4-6 tasks with `kstep_task_create()`. In `run()`, create a high-frequency sleep/wake pattern using `kstep_task_usleep()` and `kstep_task_wakeup()` on different CPUs to generate continuous PSI state transitions. Use `kstep_tick_repeat(1000)` with small tick intervals to simulate intensive scheduling. Implement an `on_tick_begin()` callback that periodically reads PSI pressure via `kstep_cgroup_write()` to `psi.pressure` files, capturing the cumulative pressure values. Log pressure deltas and detect negative values or unrealistic spikes (>1000x normal). The bug triggers when aggregators race with state changes, observable as bogus pressure spikes in the cumulative time measurements.
