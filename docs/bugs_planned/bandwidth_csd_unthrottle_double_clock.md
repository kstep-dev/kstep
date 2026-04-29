# Bandwidth: Multiple rq clock updates in CFS bandwidth async unthrottle

**Commit:** `ebb83d84e49b54369b0db67136a5fe1087124dcc`
**Affected files:** kernel/sched/fair.c, kernel/sched/sched.h
**Fixed in:** v6.5-rc1
**Buggy since:** v6.3-rc1 (introduced by commit `8ad075c2eb1f` "sched: Async unthrottling for cfs bandwidth")

## Bug Description

When CFS bandwidth throttling is enabled and multiple cgroup cfs_rq's on the same CPU become throttled, the asynchronous unthrottling mechanism introduced in commit `8ad075c2eb1f` can trigger the `WARN_DOUBLE_CLOCK` warning. This occurs because `update_rq_clock()` is called multiple times while holding the rq lock during a single critical section, which the kernel's debug infrastructure specifically warns about.

The async unthrottle mechanism was designed to avoid hard lockups on systems with many CPUs and large cgroup hierarchies. Instead of unthrottling all cfs_rq's inline in the hrtimer callback (which runs with IRQs disabled), it sends a CSD (cross-CPU call) to each CPU, letting each CPU unthrottle its own cfs_rq's. The function `__cfsb_csd_unthrottle()` iterates over a list of throttled cfs_rq's on the target CPU and calls `unthrottle_cfs_rq()` for each one. However, `unthrottle_cfs_rq()` internally calls `update_rq_clock(rq)` at the start of its execution.

When multiple cfs_rq's are queued on the same CPU's `cfsb_csd_list`, the loop in `__cfsb_csd_unthrottle()` calls `unthrottle_cfs_rq()` multiple times, resulting in multiple `update_rq_clock()` calls within the same rq lock hold. The `WARN_DOUBLE_CLOCK` debug check detects this and fires a warning. A similar (but less common) issue exists in `unthrottle_offline_cfs_rqs()`, which is called during CPU offlining and also iterates over throttled cfs_rq's calling `unthrottle_cfs_rq()` in a loop.

The warning is not just cosmetic — it indicates that the rq clock is being updated redundantly. While not a crash or data corruption issue in production kernels (the `WARN_DOUBLE_CLOCK` check is under `CONFIG_SCHED_DEBUG`), it signals a code quality issue where clock updates are not properly managed, which could mask real bugs or cause subtle timing inconsistencies in clock-sensitive accounting.

## Root Cause

The root cause lies in the interaction between two code paths:

1. **`unthrottle_cfs_rq()`** (fair.c:5487): This function starts by calling `update_rq_clock(rq)` at line 5498 to ensure the rq clock is fresh before it begins re-enqueuing entities. This is correct and necessary when `unthrottle_cfs_rq()` is called individually for a single cfs_rq.

2. **`__cfsb_csd_unthrottle()`** (fair.c:5571): This function iterates over `rq->cfsb_csd_list` using `list_for_each_entry_safe()`, calling `unthrottle_cfs_rq(cursor)` for each throttled cfs_rq. Since each call to `unthrottle_cfs_rq()` internally calls `update_rq_clock(rq)`, the rq clock is updated N times where N is the number of throttled cfs_rq's on that CPU.

The kernel's `update_rq_clock()` function (core.c:750) has a debug check:

```c
void update_rq_clock(struct rq *rq)
{
    if (rq->clock_update_flags & RQCF_ACT_SKIP)
        return;

#ifdef CONFIG_SCHED_DEBUG
    if (sched_feat(WARN_DOUBLE_CLOCK))
        SCHED_WARN_ON(rq->clock_update_flags & RQCF_UPDATED);
    rq->clock_update_flags |= RQCF_UPDATED;
#endif
    ...
}
```

The `RQCF_UPDATED` flag is set on the first call. On the second call within the same rq lock hold, `RQCF_UPDATED` is already set, triggering the `SCHED_WARN_ON`. The `RQCF_UPDATED` flag is only cleared by `rq_pin_lock()` (called inside `rq_lock()`), so within a single lock-unlock cycle, any second call to `update_rq_clock()` fires the warning.

The `RQCF_ACT_SKIP` flag (value 0x02) is the mechanism designed to suppress redundant clock updates. When set, `update_rq_clock()` returns immediately without updating the clock or triggering the warning. Before the fix, neither `__cfsb_csd_unthrottle()` nor `unthrottle_offline_cfs_rqs()` used this mechanism.

The original `__cfsb_csd_unthrottle()` (introduced by commit `8ad075c2eb1f`) simply acquired the rq lock, iterated over the list, and called `unthrottle_cfs_rq()` for each entry. The author of `8ad075c2eb1f` did not account for the fact that `unthrottle_cfs_rq()` calls `update_rq_clock()` internally, and that iterating a list of cfs_rq's would produce multiple updates. For `unthrottle_offline_cfs_rqs()`, the clock had already been updated by the caller `set_rq_offline()`, so even the first call to `update_rq_clock()` inside `unthrottle_cfs_rq()` was redundant.

## Consequence

The immediate observable consequence is a `WARN_ON_ONCE` kernel warning in the kernel log (when `CONFIG_SCHED_DEBUG` is enabled and the `WARN_DOUBLE_CLOCK` sched feature is active, which it is by default in debug builds):

```
------------[ cut here ]------------
rq->clock_update_flags & RQCF_UPDATED
WARNING: CPU: 54 PID: 0 at kernel/sched/core.c:741 update_rq_clock+0xaf/0x180
Call Trace:
 <TASK>
 unthrottle_cfs_rq+0x4b/0x300
 __cfsb_csd_unthrottle+0xe0/0x100
 __flush_smp_call_function_queue+0xaf/0x1d0
 flush_smp_call_function_queue+0x49/0x90
 do_idle+0x17c/0x270
 cpu_startup_entry+0x19/0x20
 start_secondary+0xfa/0x120
 secondary_startup_64_no_verify+0xce/0xdb
```

This warning can fire on any CPU that has multiple throttled cfs_rq's queued for async unthrottle. On systems with many cgroups with CFS bandwidth limits, this could fire frequently, flooding the kernel log. In production kernels without `CONFIG_SCHED_DEBUG`, the redundant `update_rq_clock()` calls still execute but silently — they call `sched_clock_cpu()` and update the rq clock multiple times in the same critical section, which is wasteful but not harmful.

The bug does not cause crashes, data corruption, or incorrect scheduling decisions in production. It is a code quality and efficiency issue: the rq clock is needlessly recalculated multiple times per unthrottle batch. In systems with `panic_on_warn` set (common in some production environments for catching bugs early), this warning would cause a kernel panic.

## Fix Summary

The fix introduces two new inline helper functions in `kernel/sched/sched.h`:

- **`rq_clock_start_loop_update(struct rq *rq)`**: Sets the `RQCF_ACT_SKIP` flag on `rq->clock_update_flags`. This causes subsequent `update_rq_clock()` calls to return immediately without updating the clock or triggering the `WARN_DOUBLE_CLOCK` check. It also includes a `SCHED_WARN_ON` to assert that `RQCF_ACT_SKIP` is not already set (to catch nesting bugs).

- **`rq_clock_stop_loop_update(struct rq *rq)`**: Clears the `RQCF_ACT_SKIP` flag, restoring normal clock update behavior.

In `__cfsb_csd_unthrottle()`, the fix adds an explicit `update_rq_clock(rq)` call after acquiring the rq lock but before the iteration loop, then calls `rq_clock_start_loop_update(rq)` to suppress further updates during the loop. After the loop completes, `rq_clock_stop_loop_update(rq)` clears the skip flag. This ensures the clock is updated exactly once, and all subsequent calls to `unthrottle_cfs_rq()` within the loop see a fresh clock without triggering the double-update warning.

In `unthrottle_offline_cfs_rqs()`, the same pattern is applied: `rq_clock_start_loop_update(rq)` is called before the iteration (the clock was already updated by `set_rq_offline()`), and `rq_clock_stop_loop_update(rq)` is called after. This ensures the pre-existing clock update in the caller is honored and the redundant updates inside `unthrottle_cfs_rq()` are suppressed.

The approach of adding `RQCF_ACT_SKIP` around the loop was preferred over pulling `update_rq_clock()` out of `unthrottle_cfs_rq()` because that function is called from many other places where the clock update is needed. Removing it from `unthrottle_cfs_rq()` would require adding explicit `update_rq_clock()` calls at every callsite, which would be more error-prone and harder to maintain.

## Triggering Conditions

The bug requires the following conditions to trigger:

1. **CONFIG_CFS_BANDWIDTH enabled**: This is needed for cfs_rq throttling and the async unthrottle mechanism.

2. **CONFIG_SMP enabled**: The async unthrottle via CSD only operates on SMP systems (single-CPU systems inline the unthrottle directly).

3. **CONFIG_SCHED_DEBUG enabled** (for the warning): The `WARN_DOUBLE_CLOCK` check only exists under `CONFIG_SCHED_DEBUG`. Without it, the bug manifests as silent redundant clock updates.

4. **Multiple cgroups with CFS bandwidth limits on the same CPU**: At least two task groups must have their per-CPU cfs_rq's throttled on the same CPU. When the period timer fires and distributes runtime, both cfs_rq's get queued on the same CPU's `cfsb_csd_list` for async unthrottle.

5. **Tasks in bandwidth-limited cgroups running on the same CPU**: The cfs_rq's must be throttled, which requires tasks to have consumed their allocated bandwidth quota on that CPU.

6. **The unthrottle happens via CSD (remote CPU)**: The async path is taken when `distribute_cfs_runtime()` is running on a different CPU than where the throttled cfs_rq resides. If the cfs_rq is on the same CPU as the period timer, it is unthrottled inline (no CSD), and only one `update_rq_clock()` occurs.

For the `unthrottle_offline_cfs_rqs()` path, the trigger is: CPU hotplug offline while one or more cfs_rq's are throttled on the CPU being taken offline. This is less common in practice.

To reliably trigger the bug: Create 2+ task groups with CFS bandwidth limits (e.g., `cpu.max` set to a low quota), place running tasks in each group on the same CPU, wait for the tasks to be throttled, and then let the period timer fire. The period timer on its CPU will distribute runtime and queue CSD unthrottle requests to the target CPU. When the target CPU processes the CSD, it enters `__cfsb_csd_unthrottle()` and calls `unthrottle_cfs_rq()` multiple times, triggering the warning on the second call.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP since it involves CFS bandwidth throttling and cgroup management, which kSTEP supports.

### Step 1: Topology setup
Configure QEMU with at least 2 CPUs. The bug requires SMP so that the async CSD unthrottle path is taken (the period timer fires on one CPU and sends a CSD to the other CPU where the throttled cfs_rq's reside).

```c
kstep_topo_init();
kstep_topo_apply();
```

### Step 2: Create two cgroups with bandwidth limits
Create two cgroups with low CFS bandwidth quotas so their tasks get throttled quickly:

```c
kstep_cgroup_create("grpA");
kstep_cgroup_create("grpB");
// Set a low bandwidth limit (e.g., 10ms quota per 100ms period)
// This requires writing to cpu.max via sysctl or cgroup interface
```

kSTEP provides `kstep_cgroup_create()` but bandwidth limits need to be set via sysctl or direct cgroup attribute writes. kSTEP's `kstep_sysctl_write()` or a cgroup write helper may need to be used to set `cpu.max` for each group. If kSTEP does not currently support writing `cpu.max`, a small extension to write cgroup bandwidth parameters (quota and period) would be needed — for example, a `kstep_cgroup_set_bandwidth(name, quota_us, period_us)` helper.

### Step 3: Create tasks and assign to cgroups
Create at least one task per cgroup, all pinned to the same non-CPU-0 CPU (e.g., CPU 1):

```c
struct task_struct *taskA = kstep_task_create();
kstep_task_pin(taskA, 1, 1);  // Pin to CPU 1
kstep_cgroup_add_task("grpA", taskA->pid);

struct task_struct *taskB = kstep_task_create();
kstep_task_pin(taskB, 1, 1);  // Pin to CPU 1
kstep_cgroup_add_task("grpB", taskB->pid);
```

### Step 4: Let tasks run and get throttled
Wake the tasks and let them run for enough ticks to consume their bandwidth quota:

```c
kstep_task_wakeup(taskA);
kstep_task_wakeup(taskB);
kstep_tick_repeat(200);  // Enough ticks for tasks to exhaust quota and get throttled
```

### Step 5: Observe the bug via kernel log or clock_update_flags
After the period timer fires and distributes new runtime, the CSD handler `__cfsb_csd_unthrottle()` will iterate over both cfs_rq's on CPU 1 and call `unthrottle_cfs_rq()` twice. The second call will trigger the `WARN_DOUBLE_CLOCK` warning.

To detect the bug:
- **Method 1 (kernel log)**: After running, check `dmesg` or `data/logs/latest.log` for the `WARN_ON_ONCE` message containing `rq->clock_update_flags & RQCF_UPDATED` and the `unthrottle_cfs_rq+0x4b/0x300` / `__cfsb_csd_unthrottle` call trace.
- **Method 2 (instrumentation)**: Use `KSYM_IMPORT` to access `update_rq_clock` internals and track the `clock_update_flags` field on the target rq. Before and after tick processing, read `cpu_rq(1)->clock_update_flags` to see if `RQCF_UPDATED` is set when `update_rq_clock()` is about to be called again.

### Step 6: Pass/Fail criteria
- **Buggy kernel**: The kernel log should contain the `rq->clock_update_flags & RQCF_UPDATED` warning, and the call trace should show `__cfsb_csd_unthrottle` → `unthrottle_cfs_rq` → `update_rq_clock`. Call `kstep_fail()` if this warning is detected.
- **Fixed kernel**: No such warning appears. The `rq_clock_start_loop_update()` sets `RQCF_ACT_SKIP`, causing the second and subsequent `update_rq_clock()` calls inside `unthrottle_cfs_rq()` to return immediately. Call `kstep_pass()` if no warning is detected after sufficient tick progression.

### Step 7: kSTEP extensions needed
The driver likely needs a way to set CFS bandwidth limits on cgroups. If `kstep_cgroup_set_bandwidth()` or a way to write `cpu.max` cgroup files is not available, this would need to be added as a minor extension. Alternatively, the `kstep_sysctl_write()` function could potentially be used to write to the cgroup filesystem if it supports arbitrary path writes. The bandwidth period and quota are configured via the cgroup v2 `cpu.max` file (format: "quota_us period_us") or cgroup v1 `cpu.cfs_quota_us` and `cpu.cfs_period_us` files.

### Step 8: Alternative detection approach
Since the bug is fundamentally about `update_rq_clock()` being called multiple times, another approach is to use an `on_tick_begin` or `on_sched_softirq_begin` callback to directly inspect `cpu_rq(1)->clock_update_flags` during the unthrottle process. On the buggy kernel, after the first `unthrottle_cfs_rq()` call sets `RQCF_UPDATED`, the flag will still be set when the second call enters. On the fixed kernel, `RQCF_ACT_SKIP` will be set and `update_rq_clock()` returns early.

The key challenge is timing the observation to coincide with the CSD unthrottle handler execution. Using `kstep_tick_until()` with a predicate that checks whether both cfs_rq's have been throttled, followed by more ticks to trigger the period timer and CSD unthrottle, should provide the right window. The warning itself (visible in dmesg) is the simplest and most reliable detection method.
