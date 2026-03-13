# Energy: Asymmetric Wakeup Path Skips Prev/Target CPU Checks

**Commit:** `b4c9c9f15649c98a5b45408919d1ff4fd7f5531c`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.10-rc4
**Buggy since:** v5.7-rc1 (commit b7a331615d25 "sched/fair: Add asymmetric CPU capacity wakeup scan")

## Bug Description

On asymmetric CPU capacity systems (ARM big.LITTLE, DynamIQ), the CFS wakeup fast-path `select_idle_sibling()` was introduced a dedicated code path for heterogeneous CPU topologies by commit b7a331615d25. This new code path was designed to handle systems where CPUs within the same LLC domain can have different compute capacities (e.g., DynamIQ where LITTLE and big cores share an L3 cache). The problem is that this dedicated asymmetric path completely bypassed the standard heuristics that prefer reusing the task's previous CPU (`prev`) or the target CPU when they are idle.

In the symmetric (normal) wakeup path, `select_idle_sibling()` first checks if the `target` CPU is idle, then checks if the `prev` CPU is idle and cache-affine, then checks the `recent_used_cpu`, and only then scans the full LLC domain for idle CPUs. This graduated approach minimizes unnecessary task migrations and promotes cache locality. The asymmetric path, however, jumped immediately to `select_idle_capacity()` which scans the entire `sd_asym_cpucapacity` domain without first considering whether `prev` or `target` are already suitable idle candidates.

This caused massive unnecessary task migrations on asymmetric capacity systems. When two tasks repeatedly wake each other (as in `perf bench sched pipe`), instead of sticking to their respective CPUs, each wakeup would scan the entire domain and potentially pick a different CPU. On HiKey960 with the performance governor (EAS disabled), this resulted in approximately 999,364 migrations over 50,000 iterations — essentially every wakeup caused a migration. The fix reduced this to zero migrations while improving throughput by 22%.

The bug only manifested when EAS was not active (e.g., using the performance governor) or when the system was overloaded, because in normal EAS mode, the `find_energy_efficient_cpu()` slow path handles CPU selection before `select_idle_sibling()` is reached.

## Root Cause

The root cause lies in the control flow of `select_idle_sibling()` as modified by commit b7a331615d25. The buggy code placed the asymmetric capacity check at the very beginning of the function:

```c
static int select_idle_sibling(struct task_struct *p, int prev, int target)
{
    struct sched_domain *sd;
    int i, recent_used_cpu;

    /* Buggy: early return bypasses all prev/target checks */
    if (static_branch_unlikely(&sched_asym_cpucapacity)) {
        sd = rcu_dereference(per_cpu(sd_asym_cpucapacity, target));
        if (!sd)
            goto symmetric;
        i = select_idle_capacity(p, sd, target);
        return ((unsigned)i < nr_cpumask_bits) ? i : target;
    }

symmetric:
    if (available_idle_cpu(target) || sched_idle_cpu(target))
        return target;

    if (prev != target && cpus_share_cache(prev, target) &&
        (available_idle_cpu(prev) || sched_idle_cpu(prev)))
        return prev;
    // ... recent_used_cpu check ...
    // ... LLC domain scan ...
}
```

When `sched_asym_cpucapacity` is set (which it is on any big.LITTLE or DynamIQ system), the function immediately enters the asymmetric block, calls `select_idle_capacity()`, and returns. The checks for `target` being idle, `prev` being idle, and `recent_used_cpu` being idle are all completely skipped. The `select_idle_capacity()` function iterates through all CPUs in the `sd_asym_cpucapacity` domain using `for_each_cpu_wrap(cpu, cpus, target)`, picking the first idle CPU where the task fits. Since the iteration order wraps around `target`, it does somewhat prefer nearby CPUs, but it does not give any special consideration to `prev`.

The consequence is that on each wakeup, `select_idle_capacity()` might pick a different idle CPU depending on which CPUs happen to be idle at that moment. With two tasks repeatedly waking each other (producer-consumer pattern), both tasks constantly migrate between CPUs, thrashing caches and increasing scheduler overhead.

A secondary issue was that `sync_entity_load_avg()` was called inside `select_idle_capacity()` instead of at the top level, and `task_fits_capacity()` was called per-CPU in the loop (which internally calls `uclamp_task_util()` each time), resulting in redundant computation. The fix hoists the utility computation to the top of `select_idle_sibling()` and passes the pre-computed `task_util` value.

The problem can also be understood as a violation of the principle that `select_idle_sibling()` should always consider the "obvious" candidates (target, prev, recent_used_cpu) before performing an expensive domain scan. The symmetric path was carefully designed with this principle, but the asymmetric path was added as a short-circuit that bypassed it entirely.

## Consequence

The primary consequence is excessive task migrations on asymmetric CPU capacity systems when EAS is not active or when the system is overloaded. This manifests as:

1. **Severe performance degradation**: On HiKey960 (ARM big.LITTLE with 4 LITTLE + 4 big cores) running `perf bench sched pipe` with the performance governor (EAS disabled), throughput drops by 22% compared to the fixed kernel (149,313 ops/sec vs 182,587 ops/sec). This is a substantial regression for a workload that should be straightforward to optimize.

2. **Near-total migration storm**: The same benchmark shows 999,364 migrations over 50,000 iterations (approximately 20 migrations per iteration for a 2-task pipe benchmark). With the fix, migrations drop to zero. This means every single wakeup causes a needless migration, thrashing cache lines and increasing inter-CPU communication overhead.

3. **Cache thrashing and increased latency**: Each unnecessary migration means the task must warm up its cache working set on a new CPU. For latency-sensitive workloads or producer-consumer patterns common in real applications, this translates to increased tail latency and reduced throughput. The effect is particularly pronounced on DynamIQ systems where all CPUs share a single LLC — the migration doesn't even improve cache locality, it just wastes scheduler cycles.

The bug does not cause crashes, hangs, or data corruption. It is a pure performance regression on heterogeneous CPU capacity systems. The impact is most severe on systems using the performance governor (which disables EAS) or when the system is overloaded (which also bypasses EAS). On systems where EAS is active and the system is not overloaded, the `find_energy_efficient_cpu()` slow path handles CPU selection before `select_idle_sibling()` is reached, so the bug has no impact.

## Fix Summary

The fix restructures `select_idle_sibling()` to align the asymmetric path with the symmetric one. Instead of short-circuiting to `select_idle_capacity()` at the top of the function, the fix moves the asymmetric domain scan to *after* the target/prev/recent_used_cpu checks. This ensures that on asymmetric systems, the standard "fast path" heuristics are applied first.

Specifically, the fix makes the following changes:

1. **Moves `sync_entity_load_avg()` and `uclamp_task_util()` to the top of `select_idle_sibling()`**: When `sched_asym_cpucapacity` is set, the task's load average is synced and its uclamped utilization is computed once upfront, stored in `task_util`. This avoids redundant computation.

2. **Adds `asym_fits_capacity()` guard to target/prev/recent_used_cpu checks**: A new inline helper `asym_fits_capacity(task_util, cpu)` is added. On asymmetric systems, it returns `fits_capacity(task_util, capacity_of(cpu))`, ensuring the task actually fits on the candidate CPU. On symmetric systems, it unconditionally returns `true`. This guard is added to all three fast-path checks (target, prev, recent_used_cpu).

3. **Moves the `select_idle_capacity()` call to after the fast-path checks**: The asymmetric domain scan now only triggers if none of the fast-path candidates (target, prev, recent_used_cpu) were suitable. This preserves the capacity-awareness while still preferring cache-warm CPUs.

4. **Simplifies `select_idle_capacity()`**: The function now receives a pre-synced task and uses `fits_capacity(task_util, cpu_cap)` instead of `task_fits_capacity(p, cpu_cap)`, since `task_util` is already computed by the caller.

The fix is correct because it preserves the capacity-fitness check (a task won't be placed on a CPU that's too small) while restoring the migration-reducing heuristics that prefer reusing the previous CPU. If `prev` is idle and has sufficient capacity, it is always a better choice than scanning the domain — it avoids migration overhead and cache thrashing.

## Triggering Conditions

The bug requires the following conditions:

- **Asymmetric CPU capacity topology**: The system must have CPUs with different compute capacities, such as ARM big.LITTLE or DynamIQ. The `sched_asym_cpucapacity` static key must be enabled, which happens automatically when the kernel detects asymmetric capacity in the topology.

- **EAS not active or system overloaded**: When EAS is active and the system is not overloaded, tasks are placed via `find_energy_efficient_cpu()` which bypasses `select_idle_sibling()`. The bug only manifests in the fast wakeup path, which is used when EAS is disabled (e.g., performance governor) or when the system is marked as overloaded (all CPUs busy beyond a threshold).

- **Repeated wakeup pattern (producer-consumer)**: The bug is most visible with workloads where two or more tasks repeatedly wake each other, such as `perf bench sched pipe`. Each wakeup triggers `select_idle_sibling()`, and without the prev/target preference, every wakeup can select a different CPU.

- **Multiple idle CPUs available**: For migrations to occur, there must be idle CPUs available. If the system is fully loaded with no idle CPUs, `select_idle_capacity()` returns -1 and the `target` is used regardless. The bug is most severe when the system has many idle CPUs (light load with few tasks).

- **Kernel version**: The bug exists in kernels v5.7-rc1 through v5.10-rc3 (approximately May 2020 to November 2020). It was introduced by commit b7a331615d25 and fixed by commit b4c9c9f15649.

The bug is 100% reproducible on qualifying hardware by running `perf bench sched pipe -T` with the performance governor and checking migration counts via `perf stat`.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Kernel Version Too Old

kSTEP supports Linux v5.15 and newer only. The bug was introduced in v5.7-rc1 (commit b7a331615d25) and fixed in v5.10-rc4 (commit b4c9c9f15649c98a5b45408919d1ff4fd7f5531c). All affected kernel versions (v5.7 through v5.10-rc3) are older than the v5.15 minimum supported by kSTEP. The buggy code path does not exist in any kernel version that kSTEP can build and run.

### 2. Asymmetric CPU Capacity Requirements

Even if the kernel version were supported, reproducing this bug would require an asymmetric CPU capacity topology. kSTEP provides `kstep_cpu_set_capacity()` to set per-CPU capacity values, but the `sched_asym_cpucapacity` static key and `sd_asym_cpucapacity` sched domain pointer are set up during topology initialization based on actual hardware topology detection. On x86_64 (which kSTEP/QEMU runs), asymmetric CPU capacity support was only added in v6.12 (commit series enabling `SD_ASYM_CPUCAPACITY` on x86 for Intel hybrid CPUs). For the v5.7–v5.10 timeframe, x86 had no asymmetric capacity support at all, making it impossible to trigger this code path even with `kstep_cpu_set_capacity()`.

### 3. What Would Need to Change

To reproduce this bug in kSTEP, the following would need to happen:
- kSTEP would need to support kernels older than v5.15 (specifically v5.7–v5.10-rc3)
- kSTEP would need to either run on ARM64 QEMU with big.LITTLE emulation, or backport x86 asymmetric capacity support to pre-v5.10 kernels
- The topology initialization would need to set `sched_asym_cpucapacity` and create `sd_asym_cpucapacity` domains

### 4. Alternative Reproduction Methods

The bug can be reproduced on real ARM big.LITTLE or DynamIQ hardware (e.g., HiKey960, Juno, or any modern ARM SoC with heterogeneous CPUs):

1. Set the cpufreq governor to `performance` to disable EAS: `echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor`
2. Run `perf stat -e migrations perf bench sched pipe -T -l 50000`
3. On the buggy kernel, observe ~999,000 migrations; on the fixed kernel, observe ~0 migrations
4. Alternatively, use QEMU with ARM64 emulation and a DTS file that defines a big.LITTLE topology, though performance would be extremely slow
