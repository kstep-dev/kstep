# Uclamp: UCLAMP_FLAG_IDLE Not Cleared After Active Update

**Commit:** `ca4984a7dd863f3e1c0df775ae3e744bff24c303`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.15-rc1
**Buggy since:** v5.3-rc1 (commit e496187da710 "sched/uclamp: Enforce last task's UCLAMP_MAX")

## Bug Description

The `UCLAMP_FLAG_IDLE` flag on a runqueue tracks whether the CPU has become idle from a uclamp perspective — that is, all uclamp-active tasks have been dequeued. When the last uclamp-active task is dequeued (all `bucket.tasks` counters reach 0 for UCLAMP_MAX), the flag is set to retain the last known `uclamp.max` value. This prevents blocked utilization from suddenly becoming visible, which would cause undesired CPU frequency increases on an idle CPU sharing a frequency domain with active CPUs.

The bug is an asymmetry between how `UCLAMP_FLAG_IDLE` is set and cleared. The flag is **set** inside `uclamp_rq_dec_id()` (via the `uclamp_rq_max_value()` → `uclamp_idle_value()` call chain) whenever the last active task is decremented. The flag is **cleared** only inside `uclamp_rq_inc()` — the top-level wrapper called at enqueue time — but NOT inside the lower-level `uclamp_rq_inc_id()` function. This distinction matters because `uclamp_update_active()` calls `uclamp_rq_dec_id()` and `uclamp_rq_inc_id()` directly (not through the `uclamp_rq_inc()`/`uclamp_rq_dec()` wrappers), bypassing the flag-clearing code.

When `uclamp_update_active()` is invoked on a running task (for example, when cgroup uclamp parameters change or when `sched_setattr()` is called with `SCHED_FLAG_KEEP_PARAMS`), it decrements and re-increments the uclamp accounting for each clamp ID. If the task being updated is the **only** active task in its UCLAMP_MAX bucket, the decrement transiently brings `bucket.tasks` to 0, causing `uclamp_idle_value()` to set `UCLAMP_FLAG_IDLE`. The subsequent increment via `uclamp_rq_inc_id()` restores the bucket count but does **not** clear the flag, leaving the runqueue in an inconsistent "idle" state despite having active runnable tasks.

## Root Cause

The root cause lies in the `uclamp_update_active()` function and its interaction with the flag-setting/clearing paths. In the buggy code:

```c
static inline void
uclamp_update_active(struct task_struct *p)
{
    ...
    for_each_clamp_id(clamp_id) {
        if (p->uclamp[clamp_id].active) {
            uclamp_rq_dec_id(rq, p, clamp_id);   // Can SET UCLAMP_FLAG_IDLE
            uclamp_rq_inc_id(rq, p, clamp_id);   // Does NOT clear UCLAMP_FLAG_IDLE
        }
    }
    ...
}
```

The call chain for setting the flag is: `uclamp_rq_dec_id()` → when `bucket->tasks` reaches 0, the function calls `uclamp_rq_max_value()` → if no other buckets have tasks, it calls `uclamp_idle_value()` which executes `rq->uclamp_flags |= UCLAMP_FLAG_IDLE` (line 1309 in the buggy kernel).

The call chain for clearing the flag only exists in `uclamp_rq_inc()`:
```c
static inline void uclamp_rq_inc(struct rq *rq, struct task_struct *p)
{
    ...
    for_each_clamp_id(clamp_id)
        uclamp_rq_inc_id(rq, p, clamp_id);

    /* Reset clamp idle holding when there is one RUNNABLE task */
    if (rq->uclamp_flags & UCLAMP_FLAG_IDLE)
        rq->uclamp_flags &= ~UCLAMP_FLAG_IDLE;
}
```

But `uclamp_rq_inc_id()` itself — which is what `uclamp_update_active()` calls — does NOT clear the flag. While `uclamp_rq_inc_id()` does call `uclamp_idle_reset()`, that function only resets the rq's uclamp **value** (via `WRITE_ONCE(rq->uclamp[clamp_id].value, clamp_value)`), not the flag itself.

Additionally, there is a subtle ordering issue. `for_each_clamp_id()` iterates UCLAMP_MIN first, then UCLAMP_MAX. The flag is set only when processing UCLAMP_MAX (since `uclamp_idle_value()` checks `clamp_id == UCLAMP_MAX`). So in `uclamp_update_active()`, when the dec/inc pair for UCLAMP_MIN runs, the flag is not affected. Then when the dec/inc pair for UCLAMP_MAX runs, the dec sets the flag, and the inc does not clear it. The flag persists incorrectly.

The `uclamp_update_active()` path is triggered from two places: (1) `uclamp_update_active_tasks()`, called when cgroup uclamp settings are modified via `cpu_util_update_eff()`, and (2) directly from `__sched_setattr()` when a task's uclamp values are changed at runtime. Both paths lead to the same broken state.

## Consequence

The most significant consequence is incorrect CPU frequency selection via the schedutil governor. When `UCLAMP_FLAG_IDLE` is stuck set on a runqueue that has active tasks, any subsequent task enqueue triggers `uclamp_idle_reset()` inside `uclamp_rq_inc_id()`. This function, seeing the (incorrectly) set flag, **overwrites** the aggregated `rq->uclamp[clamp_id].value` with the newly enqueued task's clamp value, instead of properly max-aggregating it with existing tasks' values. This can cause the rq-level uclamp max to be set lower than it should be, leading to CPU frequency being capped below what active tasks require. Conversely, if the retained value from the "idle" state is higher than what current tasks need, the CPU may run at an unnecessarily high frequency, wasting energy.

On Android systems (where uclamp is heavily used for energy-aware scheduling and performance tuning), this bug was reported by Rick Yiu at Google. The consequence manifests as incorrect power management: CPUs may be frequency-boosted when they should be idle-clamped, or frequency-capped when they should be running at full speed. For mobile devices, this directly impacts battery life and responsiveness.

The bug does not cause a kernel crash, oops, or data corruption. It is a logical state corruption of the runqueue's uclamp accounting flags that leads to silently incorrect scheduling behavior. The severity depends on the workload and how frequently `uclamp_update_active()` is invoked (e.g., how often cgroup uclamp knobs are tuned by userspace power managers).

## Fix Summary

The fix introduces a new helper function `uclamp_rq_reinc_id()` that wraps the dec/inc pair and explicitly clears `UCLAMP_FLAG_IDLE` after the transient decrement:

```c
static inline void uclamp_rq_reinc_id(struct rq *rq, struct task_struct *p,
                                      enum uclamp_id clamp_id)
{
    if (!p->uclamp[clamp_id].active)
        return;

    uclamp_rq_dec_id(rq, p, clamp_id);
    uclamp_rq_inc_id(rq, p, clamp_id);

    /*
     * Make sure to clear the idle flag if we've transiently reached 0
     * active tasks on rq.
     */
    if (clamp_id == UCLAMP_MAX && (rq->uclamp_flags & UCLAMP_FLAG_IDLE))
        rq->uclamp_flags &= ~UCLAMP_FLAG_IDLE;
}
```

The `uclamp_update_active()` function is then updated to call `uclamp_rq_reinc_id()` instead of directly calling the dec/inc pair. The check `clamp_id == UCLAMP_MAX` ensures the flag is only cleared after processing UCLAMP_MAX (since that's the only clamp ID that can set it). The check for `UCLAMP_FLAG_IDLE` being set ensures we only clear it when it was actually set by the transient decrement.

This fix is correct and complete because: (1) it addresses the root cause by clearing the flag at the exact point where the transient state occurs, (2) it only clears the flag for UCLAMP_MAX which is the only clamp that sets it, (3) it preserves the existing behavior for the normal enqueue/dequeue paths (which already had correct flag management via `uclamp_rq_inc()`), and (4) it does not affect tasks that are not currently active (the early return on `!p->uclamp[clamp_id].active`).

## Triggering Conditions

The bug requires the following precise conditions:

1. **CONFIG_UCLAMP_TASK=y** must be enabled in the kernel configuration. On most ARM/Android kernels this is the default, but on x86 desktop kernels it may not be enabled. Additionally, `CONFIG_UCLAMP_TASK_GROUP=y` is needed if triggering via the cgroup path.

2. **sched_uclamp_used must be active**: At least one task must have had its uclamp values set via `sched_setattr()` or cgroup uclamp knobs (`cpu.uclamp.min`, `cpu.uclamp.max`). This enables the static branch `sched_uclamp_used`.

3. **A single active task in a UCLAMP_MAX bucket**: There must be exactly one runnable task whose `uclamp[UCLAMP_MAX]` falls into a particular bucket, and no other tasks in that same bucket. When `uclamp_rq_dec_id()` decrements this task, `bucket->tasks` reaches 0, triggering the `uclamp_rq_max_value()` → `uclamp_idle_value()` path that sets `UCLAMP_FLAG_IDLE`.

4. **No other tasks in ANY UCLAMP_MAX bucket**: For `uclamp_idle_value()` to be reached, `uclamp_rq_max_value()` must find no buckets with active tasks. This means the task being updated must be the **only** runnable task on the CPU (or at least the only one with active uclamp accounting).

5. **Trigger `uclamp_update_active()`**: This can be done by either:
   - Writing to `cpu.uclamp.max` or `cpu.uclamp.min` in the task's cgroup, which triggers `cpu_util_update_eff()` → `uclamp_update_active_tasks()` → `uclamp_update_active()`.
   - Calling `sched_setattr()` on the task with changed uclamp values.

6. **No race conditions required**: This is a purely deterministic bug. Every time `uclamp_update_active()` runs on the sole active task on a CPU, the flag will be incorrectly set and not cleared. The bug triggers 100% of the time under the described conditions.

The number of CPUs and topology are not important for triggering. Even a single-CPU configuration can reproduce the bug, as long as the task is the only runnable uclamp-active task when the update occurs.

## Reproduce Strategy (kSTEP)

The following step-by-step plan describes how to build a kSTEP driver to reproduce this bug:

### Setup Phase

1. **Create one CFS task** using `kstep_task_create()`. This will be the sole task we manipulate. Pin it to CPU 1 using `kstep_task_pin(task, 1, 2)` to avoid CPU 0 (reserved for the driver). No special topology is needed; 2 CPUs suffice.

2. **Create a cgroup** with `kstep_cgroup_create("test_uclamp")` and set initial uclamp values: `kstep_cgroup_write("test_uclamp", "cpu.uclamp.max", "50.00")`. Add the task to the cgroup with `kstep_cgroup_add_task("test_uclamp", task->pid)`.

3. **Set per-task uclamp** using `sched_setattr_nocheck()` (as demonstrated in the existing `uclamp_inversion.c` driver) with a specific `sched_util_max` value (e.g., 512, which is 50% of SCHED_CAPACITY_SCALE). Set `SCHED_FLAG_UTIL_CLAMP` in `sched_flags`.

### Triggering Phase

4. **Wake the task** with `kstep_task_wakeup(task)`. This enqueues it, calling `uclamp_rq_inc()` which properly handles flags. After wakeup, verify `task->uclamp[UCLAMP_MAX].active == true`.

5. **Allow the task to run** with a few ticks: `kstep_tick_repeat(5)`. This ensures the task is fully scheduled and the uclamp accounting is stable.

6. **Record the pre-update state**: Read `cpu_rq(1)->uclamp_flags` and verify `UCLAMP_FLAG_IDLE` is NOT set (since there's an active task).

7. **Trigger `uclamp_update_active()`** by writing a new uclamp.max to the cgroup: `kstep_cgroup_write("test_uclamp", "cpu.uclamp.max", "60.00")`. This triggers `cpu_util_update_eff()` → `uclamp_update_active_tasks()` → `uclamp_update_active(task)`, which internally calls `uclamp_rq_dec_id()` then `uclamp_rq_inc_id()` for each clamp ID.

### Detection Phase

8. **Check `UCLAMP_FLAG_IDLE` after the update**: Read `cpu_rq(1)->uclamp_flags & UCLAMP_FLAG_IDLE`. On the **buggy** kernel, this will be non-zero (flag incorrectly set). On the **fixed** kernel, this will be zero (flag correctly cleared by `uclamp_rq_reinc_id()`).

9. **Pass/fail criteria**: Use `kstep_pass()` if the flag state matches expectations:
   - Buggy kernel: `rq->uclamp_flags & UCLAMP_FLAG_IDLE` is non-zero → bug detected → `kstep_fail("UCLAMP_FLAG_IDLE stuck: flags=0x%x", rq->uclamp_flags)`.
   - Fixed kernel: `rq->uclamp_flags & UCLAMP_FLAG_IDLE` is zero → `kstep_pass("UCLAMP_FLAG_IDLE correctly cleared")`.

### Additional Verification

10. **Demonstrate externally observable impact**: After the flag is incorrectly set, enqueue a **second** task (created in setup but kept blocked) with a different uclamp.max. When it enqueues, `uclamp_rq_inc_id()` will call `uclamp_idle_reset()`, which — seeing the stale `UCLAMP_FLAG_IDLE` — will overwrite `rq->uclamp[UCLAMP_MAX].value` with the second task's value instead of max-aggregating. Read `rq->uclamp[UCLAMP_MAX].value` and compare: on the buggy kernel it will be the second task's value alone; on the fixed kernel it will be the proper max-aggregated value across both tasks.

### kSTEP Compatibility Notes

This bug is fully reproducible with existing kSTEP APIs. The key capabilities used are:
- `kstep_cgroup_create()` / `kstep_cgroup_write()` for cgroup manipulation (already used in existing drivers)
- `sched_setattr_nocheck()` for setting per-task uclamp values (already used in `uclamp_inversion.c`)
- Access to `cpu_rq(cpu)->uclamp_flags` via `internal.h` for reading the flag state
- `UCLAMP_FLAG_IDLE` is defined in `kernel/sched/sched.h` and accessible through the kSTEP internal header

No kSTEP framework changes are required. The kernel must be configured with `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y`. The driver should be guarded with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0)` since the UCLAMP_FLAG_IDLE mechanism was introduced in v5.3. The QEMU configuration needs at least 2 CPUs.
