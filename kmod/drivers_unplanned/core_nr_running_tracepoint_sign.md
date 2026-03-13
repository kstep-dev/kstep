# Core: nr_running Tracepoint Reports Wrong Sign on Task Dequeue

**Commit:** `a1bd06853ee478d37fae9435c5521e301de94c67`
**Affected files:** kernel/sched/sched.h
**Fixed in:** v5.9-rc1
**Buggy since:** v5.8-rc4 (introduced by commit `9d246053a691` "sched: Add a tracepoint to track rq->nr_running")

## Bug Description

The `sched_update_nr_running` tracepoint was introduced in commit `9d246053a691` to allow tracing tools (e.g., `perf`, `ftrace`, `bpftrace`) to observe changes to a run queue's `rq->nr_running` counter. This counter tracks the total number of runnable tasks on a given CPU's run queue. The tracepoint is designed to fire every time `nr_running` is incremented (task enqueue) or decremented (task dequeue), passing a signed `count` parameter that indicates the direction of the change: positive for additions and negative for subtractions.

However, the original implementation of `sub_nr_running()` contained a bug: it passed the raw `count` value (always positive, as the parameter type is `unsigned`) to `call_trace_sched_update_nr_running()` without negating it. This means that when a task was dequeued from a run queue, the tracepoint would report a positive count (e.g., `+1`) instead of a negative count (e.g., `-1`). The `add_nr_running()` function correctly passed the positive count, so only the subtraction path was affected.

This bug makes the tracepoint data misleading for any consumer. A tool monitoring the tracepoint would see every update to `nr_running` reported as a positive delta, making it impossible to distinguish between task enqueues and dequeues. Users relying on this tracepoint for debugging scheduling anomalies, runqueue depth analysis, or performance monitoring would receive incorrect data that shows `nr_running` as only ever increasing.

## Root Cause

The root cause is a missing negation in the `sub_nr_running()` inline function defined in `kernel/sched/sched.h`. Specifically, on line 2002 of the original code:

```c
static inline void sub_nr_running(struct rq *rq, unsigned count)
{
    rq->nr_running -= count;
    if (trace_sched_update_nr_running_tp_enabled()) {
        call_trace_sched_update_nr_running(rq, count);  // BUG: should be -count
    }

    /* Check if we still need preemption */
    sched_update_tick_dependency(rq);
}
```

The `count` parameter is of type `unsigned`, which is always non-negative. When `sub_nr_running()` is called (typically with `count = 1`), the actual `rq->nr_running` field is correctly decremented by `count`. However, the value passed to the tracepoint callback `call_trace_sched_update_nr_running()` is the raw positive `count`, not the negated value `-count`.

In contrast, the `add_nr_running()` function correctly passes `count` as-is, because additions are already positive:

```c
static inline void add_nr_running(struct rq *rq, unsigned count)
{
    unsigned prev_nr = rq->nr_running;
    rq->nr_running = prev_nr + count;
    if (trace_sched_update_nr_running_tp_enabled()) {
        call_trace_sched_update_nr_running(rq, count);  // Correct: positive for add
    }
    ...
}
```

The asymmetry between the two functions makes the bug clear: `add_nr_running()` reports `+count` (correct) and `sub_nr_running()` also reports `+count` (incorrect — should be `-count`). The `call_trace_sched_update_nr_running()` function's second parameter is typed as `int` to accept signed values, so passing `-count` (which implicitly converts the unsigned `count` to a negative signed value) is the intended usage.

The bug was introduced in the very same commit (`9d246053a691`) that added the tracepoint. It was a simple oversight — the developer added the same tracepoint call pattern to both `add_nr_running()` and `sub_nr_running()` but forgot to negate the count in the subtraction case.

## Consequence

The observable impact of this bug is limited to tracing infrastructure. It does **not** affect actual scheduling behavior — `rq->nr_running` is correctly updated in both functions. The bug only causes the `sched_update_nr_running` tracepoint to report incorrect data.

Specifically, any tracing tool (perf, ftrace, bpftrace, or custom BPF programs) that consumes the `sched_update_nr_running_tp` tracepoint would observe that every update to `nr_running` appears as a positive delta. This makes it impossible to determine whether a task was enqueued or dequeued by examining the tracepoint data alone. Tools that compute cumulative `nr_running` by summing the count deltas would see the counter diverge from reality, growing monotonically rather than tracking the actual number of runnable tasks.

This is particularly problematic for performance analysis and debugging scenarios where developers rely on tracepoint data to understand runqueue behavior — for example, diagnosing load imbalance across CPUs, investigating scheduling latency spikes, or validating load balancer decisions. The incorrect sign would make such analyses unreliable and potentially lead to wrong conclusions about scheduler behavior.

## Fix Summary

The fix is a one-character change in `kernel/sched/sched.h`. In the `sub_nr_running()` function, the tracepoint call is changed from:

```c
call_trace_sched_update_nr_running(rq, count);
```

to:

```c
call_trace_sched_update_nr_running(rq, -count);
```

This ensures that when tasks are removed from a run queue, the tracepoint reports a negative count (e.g., `-1`), correctly indicating a subtraction. The negation converts the `unsigned count` to a negative `int` value through implicit type conversion, which is the intended behavior since the second parameter of `call_trace_sched_update_nr_running()` accepts a signed `int`.

The fix is correct and complete because it restores the semantic contract of the tracepoint: positive values indicate additions to `nr_running` and negative values indicate subtractions. This matches the behavior that tracing consumers expect and that was clearly the original intent of the tracepoint design (as evidenced by the parameter being typed as signed `int` in the trace function prototype).

## Triggering Conditions

To observe this bug, the following conditions must be met:

- **Kernel version**: The kernel must include commit `9d246053a691` (which adds the tracepoint) but not include the fix `a1bd06853ee`. In practice, this means kernels between v5.8-rc4 and v5.9-rc1 (a very narrow window during development).
- **Tracepoint enabled**: The `sched_update_nr_running_tp` tracepoint must be actively probed. The bug only manifests when the tracepoint has consumers registered; the check `trace_sched_update_nr_running_tp_enabled()` gates the tracepoint call.
- **Task dequeue**: Any task dequeue operation triggers the bug. This includes: a task blocking (sleeping), a task being migrated to another CPU, a task exiting, or any other operation that calls `sub_nr_running()`. Since task dequeues happen continuously during normal system operation, the bug fires on every single dequeue event when the tracepoint is active.
- **No special configuration**: No special kernel configuration is needed beyond having `CONFIG_TRACEPOINTS=y` (which is enabled by default in virtually all kernel builds). No special hardware, CPU count, or topology is required.

The bug is 100% deterministic and reproducible whenever the tracepoint is active and a task is dequeued. There is no race condition or timing sensitivity — every call to `sub_nr_running()` with the tracepoint enabled will produce the wrong sign.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Kernel Version Too Old

The primary reason this bug cannot be reproduced with kSTEP is that it exists only in kernels between v5.8-rc4 and v5.9-rc1. kSTEP supports Linux v5.15 and newer only. The buggy code was introduced by commit `9d246053a691` (merged around v5.8-rc4, June 2020) and fixed by commit `a1bd06853ee` (merged into v5.9-rc1, August 2020). Both commits predate v5.15 (released October 2021) by over a year. Attempting to build and run kSTEP against a v5.8 or v5.9-rc kernel would fail due to incompatible internal APIs and missing features that kSTEP depends on.

### 2. Bug is Tracing-Only (No Scheduling Impact)

Even if the kernel version were supported, this bug does not affect any scheduling behavior. The `rq->nr_running` counter is correctly updated in both the buggy and fixed kernels. The bug only affects the data reported through the `sched_update_nr_running_tp` tracepoint. kSTEP's observation facilities (`kstep_output_nr_running()`, `kstep_output_curr_task()`, etc.) read the actual scheduler state, not tracepoint output. Therefore, kSTEP cannot distinguish between buggy and fixed behavior through its standard observation APIs.

### 3. What Would Be Needed

To reproduce this bug with kSTEP (ignoring the version constraint), the framework would need:
- **Tracepoint probe registration**: A new API like `kstep_tracepoint_register(name, probe_fn)` that allows registering a probe function on an arbitrary kernel tracepoint from within the kSTEP driver. The probe function would receive the tracepoint arguments and could inspect them.
- **Tracepoint argument inspection**: The ability to read the `count` parameter from the `sched_update_nr_running` tracepoint callback and verify its sign.

While registering tracepoint probes from a kernel module is technically possible (using `tracepoint_probe_register()`), kSTEP does not currently provide a helper for this, and the primary barrier remains the kernel version requirement.

### 4. Alternative Reproduction Methods

Outside of kSTEP, this bug can be trivially reproduced on a v5.8-rc4 to v5.9-rc1 kernel using:

1. **ftrace**: Enable the `sched_update_nr_running` tracepoint via `/sys/kernel/debug/tracing/events/sched/sched_update_nr_running_tp/enable` and read the trace buffer. Observe that the `count` field is always positive, even for task dequeue events.

2. **bpftrace**: Run `bpftrace -e 'tracepoint:sched:sched_update_nr_running { printf("cpu=%d nr=%d count=%d\n", args->cpu, args->nr_running, args->count); }'` and observe that `count` is never negative.

3. **perf**: Use `perf record -e sched:sched_update_nr_running -a -- sleep 1` and inspect the recorded events with `perf script`. Verify that the count field incorrectly shows positive values for all events.

In all cases, the expected behavior on the fixed kernel is that `count` is negative when `nr_running` decreases (task dequeue) and positive when `nr_running` increases (task enqueue).
