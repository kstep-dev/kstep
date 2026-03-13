# Deadline: Stale extra_bw After Root Domain Rebuild

**Commit:** `fcc9276c4d331cd1fe9319d793e80b02e09727f5`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.17-rc1
**Buggy since:** v6.15-rc1 (commit `2ff899e3516437354204423ef0a94994717b8e6a` "sched/deadline: Rebuild root domain accounting after every update")

## Bug Description

When root domains are rebuilt (e.g., during cpuset changes, CPU hotplug, or suspend/resume), the function `dl_clear_root_domain()` resets the root domain's `total_bw` to zero and then re-adds bandwidth contributions from each CPU's dl-server (the `fair_server`). However, it fails to reset the per-runqueue `extra_bw` variable, which tracks the spare reclaimable bandwidth available on each CPU. This means `extra_bw` retains stale values from the previous domain configuration, and the subsequent `__dl_add()` calls for dl-servers subtract bandwidth from an already-stale base rather than from a clean `max_bw` value.

This bug was introduced when commit `2ff899e351643` refactored root domain DL accounting to clear and rebuild it after every domain update. The refactored `dl_clear_root_domain()` correctly reset `total_bw` and correctly re-added dl-server contributions via `__dl_add()`, but it overlooked the fact that `__dl_add()` internally calls `__dl_update()`, which modifies each runqueue's `extra_bw`. Without first resetting `extra_bw` to `max_bw`, the extra_bw values drift further from their correct values with each domain rebuild.

The impact is most visible when GRUB (Greedy Reclamation of Unused Bandwidth) is active via `SCHED_FLAG_RECLAIM`. GRUB uses `extra_bw` to determine how aggressively to reclaim spare bandwidth. With stale `extra_bw` values, the GRUB algorithm computes incorrect runtime scaling, causing SCHED_DEADLINE tasks to consume their runtime budget too quickly and miss their deadlines. The original reporter observed 43 million deadline misses on an Intel NUC and 600 thousand on a RADXA ROCK5B when running `SCHED_DEADLINE` tasks with reclaim mode enabled.

## Root Cause

The root cause lies in the interaction between `dl_clear_root_domain()`, `__dl_add()`, and `__dl_update()` in `kernel/sched/deadline.c`.

When `dl_clear_root_domain()` is called (via `dl_rebuild_rd_accounting()` from `partition_sched_domains_locked()`), the buggy code does:
```c
void dl_clear_root_domain(struct root_domain *rd)
{
    int i;
    guard(raw_spinlock_irqsave)(&rd->dl_bw.lock);
    rd->dl_bw.total_bw = 0;  // Reset total_bw

    // Re-add dl-server contributions
    for_each_cpu(i, rd->span) {
        struct sched_dl_entity *dl_se = &cpu_rq(i)->fair_server;
        if (dl_server(dl_se) && cpu_active(i))
            __dl_add(&rd->dl_bw, dl_se->dl_bw, dl_bw_cpus(i));
    }
}
```

The function `__dl_add()` does two things: it adds `tsk_bw` to `total_bw`, and it calls `__dl_update(dl_b, -((s32)tsk_bw / cpus))`, which iterates over all CPUs in the root domain and subtracts `tsk_bw / cpus` from each CPU's `rq->dl.extra_bw`. This subtraction represents the bandwidth now "reserved" by the dl-server and no longer available as spare.

The problem is that `extra_bw` was never reset before this loop. During initialization, `extra_bw` is set to `max_bw` (the maximum reclaimable bandwidth per CPU). After the first domain build, `extra_bw = max_bw - server_bw_per_cpu`, which is correct. But on the *second* domain rebuild, `total_bw` is reset to 0, yet `extra_bw` is left at its current value (`max_bw - server_bw_per_cpu`). Then `__dl_add()` subtracts `server_bw_per_cpu` again, leaving `extra_bw = max_bw - 2 * server_bw_per_cpu`. Each subsequent rebuild subtracts yet another `server_bw_per_cpu`, causing `extra_bw` to monotonically decrease toward zero (or even underflow).

This stale `extra_bw` value directly feeds into `grub_reclaim()`:
```c
static u64 grub_reclaim(u64 delta, struct rq *rq, struct sched_dl_entity *dl_se)
{
    u64 u_inact = rq->dl.this_bw - rq->dl.running_bw;

    if (u_inact + rq->dl.extra_bw > rq->dl.max_bw - dl_se->dl_bw)
        u_act = dl_se->dl_bw;
    else
        u_act = rq->dl.max_bw - u_inact - rq->dl.extra_bw;

    u_act = (u_act * rq->dl.bw_ratio) >> RATIO_SHIFT;
    return (delta * u_act) >> BW_SHIFT;
}
```

When `extra_bw` is too small (stale), the else branch computes a larger `u_act` (active utilization), which means each unit of wall-clock time consumes a larger unit of runtime. This causes SCHED_DEADLINE tasks using GRUB to exhaust their runtime budget more quickly than they should, triggering deadline misses.

## Consequence

The primary consequence is massive deadline misses for SCHED_DEADLINE tasks using `SCHED_FLAG_RECLAIM` (GRUB). The original reporter Marcel Ziswiler observed 43 million deadline misses on an Intel NUC and 600 thousand deadline misses on a RADXA ROCK5B over the course of a week of testing. These systems normally show zero deadline misses for 5ms-granularity deadline tasks. The bug was triggered by simply adding two overrunning jobs with reclaim mode enabled to the job mix.

Without GRUB enabled, the bug still corrupts `extra_bw` accounting, but the impact is invisible because `grub_reclaim()` is not called for non-reclaiming tasks. However, the stale `extra_bw` could potentially affect deadline admission control if it uses the same accounting, leading to incorrect bandwidth capacity calculations.

The severity is high for real-time workloads relying on GRUB reclamation, as it renders the reclamation mechanism counterproductive—instead of allowing tasks to benefit from spare bandwidth, it causes them to miss deadlines. The bug is triggered deterministically on every root domain rebuild, and the degradation worsens with each successive rebuild (e.g., each cpuset change, each suspend/resume cycle).

## Fix Summary

The fix adds two lines to `dl_clear_root_domain()` that reset each CPU's `extra_bw` to `max_bw` before re-adding dl-server contributions:

```c
void dl_clear_root_domain(struct root_domain *rd)
{
    int i;
    guard(raw_spinlock_irqsave)(&rd->dl_bw.lock);

    /* Reset total_bw to zero and extra_bw to max_bw so that next
     * loop will add dl-servers contributions back properly */
    rd->dl_bw.total_bw = 0;
    for_each_cpu(i, rd->span)
        cpu_rq(i)->dl.extra_bw = cpu_rq(i)->dl.max_bw;

    /* Re-add dl-server contributions */
    for_each_cpu(i, rd->span) {
        struct sched_dl_entity *dl_se = &cpu_rq(i)->fair_server;
        if (dl_server(dl_se) && cpu_active(i))
            __dl_add(&rd->dl_bw, dl_se->dl_bw, dl_bw_cpus(i));
    }
}
```

By resetting `extra_bw` to `max_bw` first, the subsequent `__dl_add()` calls correctly subtract dl-server bandwidth from a clean baseline. After the loop, `extra_bw = max_bw - (total dl-server bw / cpus)`, which is exactly the amount of spare bandwidth available for reclamation. This ensures that regardless of how many times root domains are rebuilt, the `extra_bw` value always reflects the correct spare bandwidth.

The fix is both minimal and complete: it addresses the root cause (missing reset) without changing the overall flow. The `dl_add_task_root_domain()` function, which is called later to add real SCHED_DEADLINE task contributions, will further adjust `extra_bw` correctly because it too uses `__dl_add()`.

## Triggering Conditions

1. **Kernel version**: v6.15-rc1 through v6.16.x (any kernel containing commit `2ff899e351643` but not `fcc9276c4d33`).

2. **Root domain rebuild**: The bug requires at least two root domain rebuilds. The first build sets `extra_bw` correctly, but the second one corrupts it. Any of the following triggers a rebuild:
   - CPU hotplug (online/offline)
   - Suspend/resume cycle (`cpuset_cpu_active()` / `cpuset_cpu_inactive()`)
   - Cpuset configuration changes (`rebuild_sched_domains()`)
   - Topology updates (e.g., from `kstep_topo_apply()` calling `rebuild_sched_domains()`)

3. **dl-server presence**: The `fair_server` (a dl_server) must be active on at least one CPU. The fair_server is enabled by default on modern kernels when `CONFIG_SMP` is set, so this condition is normally met on any multi-CPU system.

4. **Observability of the impact**: To observe deadline misses (the user-visible impact), SCHED_DEADLINE tasks with `SCHED_FLAG_RECLAIM` must be running. Without GRUB-enabled tasks, the stale `extra_bw` accounting can still be detected by directly inspecting the per-runqueue `dl.extra_bw` field.

5. **Multi-CPU system**: The system must have at least 2 CPUs (since CPU 0 is typically reserved in kSTEP). The `__dl_update()` function iterates over all CPUs in the root domain's span, so more CPUs means the per-CPU bandwidth share of the dl-server is smaller, but the stale accumulation still occurs.

6. **No race condition required**: The bug is entirely deterministic. Every call to `dl_clear_root_domain()` after the first one corrupts `extra_bw`. No timing sensitivity or concurrency is required.

## Reproduce Strategy (kSTEP)

The reproduction strategy leverages the fact that `kstep_topo_apply()` calls `rebuild_sched_domains()`, which triggers `partition_sched_domains_locked()` → `dl_rebuild_rd_accounting()` → `dl_clear_root_domain()`.

### Step 1: Setup (2+ CPUs)

Configure QEMU with at least 2 CPUs. No special topology is required—a flat SMP topology suffices. No cgroup configuration is needed.

```c
// No tasks needed - the bug is in root domain accounting for dl-servers
// which exist on every CPU by default
```

### Step 2: Record initial extra_bw values

Before triggering any rebuild, read the baseline `extra_bw` and `max_bw` for each CPU using internal kernel state access:

```c
#include "internal.h"
// Access cpu_rq(i)->dl.extra_bw and cpu_rq(i)->dl.max_bw
for_each_online_cpu(i) {
    struct rq *rq = cpu_rq(i);
    u64 extra_bw_initial = rq->dl.extra_bw;
    u64 max_bw = rq->dl.max_bw;
    // Record these values
}
```

The initial state should have `extra_bw` equal to `max_bw` minus the per-CPU dl-server bandwidth share (from the boot-time domain build).

### Step 3: Trigger first root domain rebuild

Call `kstep_topo_apply()` to trigger a domain rebuild. After the call, read `extra_bw` again:

```c
kstep_topo_apply();
kstep_sleep(); // Allow rebuild to complete

for_each_online_cpu(i) {
    u64 extra_bw_after_1st = cpu_rq(i)->dl.extra_bw;
    // On fixed kernel: extra_bw == max_bw - server_share (correct)
    // On buggy kernel: extra_bw == previous_extra_bw - server_share (drifted)
}
```

### Step 4: Trigger second root domain rebuild

Call `kstep_topo_apply()` again. This is where the bug becomes clearly visible:

```c
kstep_topo_apply();
kstep_sleep();

for_each_online_cpu(i) {
    u64 extra_bw_after_2nd = cpu_rq(i)->dl.extra_bw;
    u64 max_bw = cpu_rq(i)->dl.max_bw;
    // On fixed kernel: extra_bw == same as after 1st rebuild (stable)
    // On buggy kernel: extra_bw has decreased further (drifted more)
}
```

### Step 5: Pass/Fail Criteria

Compare `extra_bw` values after the first and second rebuilds. On the **fixed kernel**, they should be identical—each rebuild resets `extra_bw` to `max_bw` before subtracting dl-server bandwidth, so the result is always the same. On the **buggy kernel**, `extra_bw` after the second rebuild will be smaller than after the first, because the dl-server bandwidth was subtracted from a stale base.

```c
for_each_online_cpu(i) {
    if (extra_bw_after_2nd != extra_bw_after_1st) {
        kstep_fail("CPU %d: extra_bw drifted from %llu to %llu (max_bw=%llu)",
                   i, extra_bw_after_1st, extra_bw_after_2nd, max_bw);
    } else {
        kstep_pass("CPU %d: extra_bw stable at %llu (max_bw=%llu)",
                   i, extra_bw_after_2nd, max_bw);
    }
}
```

### Step 6: Kernel version guard

Guard the driver with `#if LINUX_VERSION_CODE` to ensure it only runs on kernels >= v6.15 where the buggy `dl_clear_root_domain()` logic exists (specifically, the version containing commit `2ff899e351643`).

### Expected behavior

- **Buggy kernel (v6.15-rc1 to v6.16.x)**: `extra_bw` decreases by `server_bw / cpus` with each `kstep_topo_apply()` call. After N rebuilds, `extra_bw ≈ max_bw - N * (server_bw / cpus)`. The driver should report `kstep_fail`.
- **Fixed kernel (v6.17-rc1+)**: `extra_bw` remains stable at `max_bw - (server_bw / cpus)` regardless of how many rebuilds occur. The driver should report `kstep_pass`.

### Additional notes

- The fair_server's `dl_bw` is set during kernel initialization. Its value depends on the global DL bandwidth limit (controlled by `/proc/sys/kernel/sched_rt_runtime_us` and `/proc/sys/kernel/sched_rt_period_us`). The default is typically runtime=950000, period=1000000 (95% bandwidth).
- No SCHED_DEADLINE user tasks are needed to observe the accounting bug. The dl-server (fair_server) alone is sufficient to trigger the stale accumulation.
- For stronger validation, repeat the rebuild 5–10 times and verify that `extra_bw` monotonically decreases on the buggy kernel but stays constant on the fixed kernel.
- If a future kSTEP extension adds `kstep_task_dl(p, runtime, deadline, period, flags)` support, the driver could be extended to also verify the observable deadline miss behavior by running DL tasks with `SCHED_FLAG_RECLAIM`.
