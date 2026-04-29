# Uclamp: Incorrect uclamp_rq Initialization Zeros UCLAMP_MAX

**Commit:** `d81ae8aac85ca2e307d273f6dc7863a721bf054e`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.9-rc1
**Buggy since:** v5.3-rc1 (introduced by `69842cba9ace` "sched/uclamp: Add CPU's clamp buckets refcounting")

## Bug Description

The `struct uclamp_rq` on each CPU's runqueue was initialized during boot by `init_uclamp()` using a blanket `memset()` to zero. The original assumption was that when the first task enqueues on a runqueue, `uclamp_rq_inc()` would properly set the `rq->uclamp[UCLAMP_MIN].value` and `rq->uclamp[UCLAMP_MAX].value` fields to their correct default values (0 for UCLAMP_MIN, SCHED_CAPACITY_SCALE i.e. 1024 for UCLAMP_MAX).

However, this lazy initialization strategy becomes fatally broken when combined with the companion patch (patch 2/2 in the same series, commit `46609ce22703` "sched/uclamp: Protect uclamp fast path code with static key") which introduces a static key `sched_uclamp_used` that skips `uclamp_rq_inc()` and `uclamp_rq_dec()` in the enqueue/dequeue fast path until userspace explicitly uses uclamp. Because `uclamp_rq_inc()` is never called when the static key is disabled, the `rq->uclamp[UCLAMP_MAX].value` remains at 0 from the `memset()`. This effectively caps every CPU's maximum utilization to 0.

The consequence is that the schedutil CPUfreq governor, which consults `uclamp_rq_util_with()` to determine frequency scaling, sees a UCLAMP_MAX of 0 on every runqueue and never requests any frequency increase. The system is effectively stuck at minimum frequency, causing severe performance degradation.

This patch is part of a 2-patch series titled "sched: Optionally skip uclamp logic in fast path" addressing a netperf UDP_STREAM regression reported by Mel Gorman on a 2-socket Xeon E5 2x10-core system when uclamp was compiled in.

## Root Cause

The root cause is in `init_uclamp()` within `kernel/sched/core.c`. The buggy code performed:

```c
for_each_possible_cpu(cpu) {
    memset(&cpu_rq(cpu)->uclamp, 0,
            sizeof(struct uclamp_rq)*UCLAMP_CNT);
    cpu_rq(cpu)->uclamp_flags = 0;
}
```

This zeroes both `rq->uclamp[UCLAMP_MIN]` and `rq->uclamp[UCLAMP_MAX]` entirely. For UCLAMP_MIN, a value of 0 is the correct default (no minimum utilization requirement). However, for UCLAMP_MAX, a value of 0 means "cap utilization at zero" — the exact opposite of the correct default, which should be SCHED_CAPACITY_SCALE (1024), meaning "no cap."

The function `uclamp_none()` defines the correct defaults:

```c
static inline unsigned int uclamp_none(enum uclamp_id clamp_id)
{
    if (clamp_id == UCLAMP_MIN)
        return 0;
    return SCHED_CAPACITY_SCALE;
}
```

Without the static key patch (patch 2/2), this was not a problem because the first call to `uclamp_rq_inc()` during `enqueue_task()` would invoke `uclamp_rq_inc_id()` which aggregates clamp values from enqueued tasks and properly updates `rq->uclamp[].value`. But when the static key is disabled (the default state until userspace uses uclamp), `uclamp_rq_inc()` is never invoked, and the zeroed UCLAMP_MAX value persists indefinitely.

The schedutil governor calls `uclamp_rq_util_with()` to clamp the runqueue's utilization before computing a target frequency. With `rq->uclamp[UCLAMP_MAX].value == 0`, the clamped utilization is always 0, and no frequency change is ever requested.

## Consequence

The observable impact is that all CPUs are effectively capped at zero utilization for frequency scaling purposes. The schedutil governor will never request frequency increases because `uclamp_rq_util_with()` returns 0 (or near-zero) utilization for every runqueue. This results in:

1. **Complete frequency scaling failure**: The system stays at minimum CPU frequency regardless of load, causing catastrophic performance degradation for all workloads.
2. **Silent regression**: There is no warning, oops, or error message. The system simply runs extremely slowly, making it very difficult to diagnose.
3. **Affects all distro kernels with uclamp enabled**: Since the static key is specifically designed to let distros ship uclamp-capable kernels without runtime overhead, any kernel that includes both patches (which is the intended usage) would exhibit this behavior until userspace first touches a uclamp knob.

The netperf UDP_STREAM benchmarks on the 2-socket Xeon E5 showed a 1-3% regression even without this particular bug, purely from the uclamp code being in the enqueue/dequeue path. The static key mechanism (patch 2/2) was the solution, but it depended on this initialization fix to work correctly.

## Fix Summary

The fix introduces a new function `init_uclamp_rq()` that replaces the `memset()` with proper per-clamp-id initialization using C99 designated initializers:

```c
static void __init init_uclamp_rq(struct rq *rq)
{
    enum uclamp_id clamp_id;
    struct uclamp_rq *uc_rq = rq->uclamp;

    for_each_clamp_id(clamp_id) {
        uc_rq[clamp_id] = (struct uclamp_rq) {
            .value = uclamp_none(clamp_id)
        };
    }

    rq->uclamp_flags = 0;
}
```

This sets `rq->uclamp[UCLAMP_MIN].value = 0` and `rq->uclamp[UCLAMP_MAX].value = SCHED_CAPACITY_SCALE` (1024). All other fields in `struct uclamp_rq` (like the bucket array) are implicitly zeroed by the C99 compound literal, which is the correct default for them (zero tasks in all buckets).

The `init_uclamp()` function is updated to call `init_uclamp_rq()` instead of memset:

```c
for_each_possible_cpu(cpu)
    init_uclamp_rq(cpu_rq(cpu));
```

This fix is correct and complete because it ensures that even when `uclamp_rq_inc()` is never called (due to the static key being off), the runqueue's uclamp values reflect the correct "no restriction" defaults. The schedutil governor will see UCLAMP_MAX=1024 (full scale) and compute frequencies normally.

## Triggering Conditions

The bug requires the following conditions to manifest:

- **Kernel version**: v5.3 through v5.8.x (the bug was introduced in v5.3-rc1 by commit 69842cba9ace and fixed in v5.9-rc1).
- **CONFIG_UCLAMP_TASK=y**: Uclamp must be compiled into the kernel.
- **Static key patch applied**: The companion patch (2/2) that adds `sched_uclamp_used` static key must also be applied. Without it, `uclamp_rq_inc()` is always called and properly initializes values on first enqueue.
- **No userspace uclamp usage**: The static key remains disabled (default) because no process has called `sched_setattr()` with uclamp parameters, no admin has modified `sysctl_sched_uclamp_util_{min,max}`, and no cgroup `cpu.uclamp.{min,max}` has been modified.
- **Schedutil governor active**: The frequency scaling impact only manifests when the schedutil CPUfreq governor is active, since it consults `uclamp_rq_util_with()`.

The bug is 100% deterministic and triggers immediately at boot on any system meeting the above conditions. It does not require any specific number of CPUs, any particular topology, or any specific workload.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD**: The bug exists only in kernel versions v5.3 through v5.8.x. The fix was merged into v5.9-rc1. kSTEP supports Linux v5.15 and newer only. By v5.15, this bug has been fixed for over a year. There is no kernel version within kSTEP's supported range that contains this bug.

2. **Boot-time initialization bug**: Even if the kernel version were supported, this is a bug in `init_uclamp()`, a function marked `__init` that runs during kernel boot. kSTEP loads as a kernel module after boot completes. By the time any kSTEP driver runs, `init_uclamp()` has already executed (with the buggy or fixed code). A kSTEP driver cannot re-invoke `init_uclamp()` or alter the boot-time initialization of `struct uclamp_rq`.

3. **Requires cpufreq/schedutil**: The observable consequence of this bug is that the schedutil governor fails to scale CPU frequency. QEMU does not have a cpufreq driver or hardware, so even if the runqueue's UCLAMP_MAX value were observable as 0, the frequency scaling failure could not be demonstrated.

4. **What would be needed**: To reproduce this bug, kSTEP would need to support kernel versions before v5.15 (specifically v5.3-v5.8 range), have a mechanism to hook into or observe boot-time initialization code, and ideally have a cpufreq subsystem to observe the scheduling impact. None of these are feasible additions.

5. **Alternative reproduction**: Outside kSTEP, this bug can be reproduced by building a v5.8 kernel with CONFIG_UCLAMP_TASK=y, applying only the static key patch (2/2) without this initialization fix (1/2), booting with schedutil as the cpufreq governor, and observing that CPU frequency never scales above minimum. The `rq->uclamp[UCLAMP_MAX].value` can be inspected via `/proc/schedstat` or tracepoints to confirm it is 0.
