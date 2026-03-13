# Uclamp: Per-CPU Kthread Wakee Stacking Ignores Capacity on Asymmetric Systems

**Commit:** `014ba44e8184e1acf93e0cbb7089ee847802f8f0`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.17-rc1
**Buggy since:** v5.10-rc4 (introduced by commit b4c9c9f15649 "sched/fair: Prefer prev cpu in asymmetric wakeup path")

## Bug Description

In `select_idle_sibling()`, there is a special fast-path for tasks woken up by a per-CPU kthread. When a per-CPU kthread (e.g., an IO completion kworker) wakes a task, and the task's previous CPU matches the kthread's CPU, and the kthread's CPU has at most one running task, the scheduler assumes the wakeup is essentially a sync wakeup (the wakee queued work for the kthread, which is now complete) and returns the previous CPU immediately without further searching. This is an optimization to avoid unnecessary task migration for patterns like IO completions.

Commit b4c9c9f15649 ("sched/fair: Prefer prev cpu in asymmetric wakeup path") restructured `select_idle_sibling()` to add capacity-fitness checks (`asym_fits_capacity()`) to the target CPU check, the previous CPU check, and the recently-used CPU check. These checks ensure that on asymmetric CPU capacity systems (e.g., ARM big.LITTLE), a task is not placed on a CPU whose capacity is too small for the task's utilization requirements. However, b4c9c9f15649 **missed** the per-CPU kthread stacking path, leaving it without any capacity-fitness check.

The omission becomes a real problem when uclamp (utilization clamping) is in play. The per-CPU kthread stacking path was designed under the assumption that a wakee's utilization during task placement cannot exceed its utilization from the previous activation. This assumption is violated by `uclamp.min`: between two task activations, a task's `uclamp.min` can be changed (e.g., via `sched_setattr()` or cgroup `cpu.uclamp.min`), causing `uclamp_task_util()` to return a significantly higher value than the task's actual historical utilization. With a high `uclamp.min`, `asym_fits_capacity()` would reject a LITTLE CPU, but the kthread stacking path bypasses this check entirely.

The result is that on an asymmetric CPU capacity system, a per-CPU kthread on a LITTLE CPU can cause a task with a high `uclamp.min` requirement to be placed on that LITTLE CPU, even though the task demands a BIG CPU. This defeats the purpose of both uclamp and the asymmetric capacity awareness in the scheduler.

## Root Cause

The root cause is a missing `asym_fits_capacity()` check in the per-CPU kthread stacking path of `select_idle_sibling()`. The relevant code in the buggy kernel (v5.10 through v5.16) is:

```c
static int select_idle_sibling(struct task_struct *p, int prev, int target)
{
    unsigned long task_util;

    if (static_branch_unlikely(&sched_asym_cpucapacity)) {
        sync_entity_load_avg(&p->se);
        task_util = uclamp_task_util(p);
    }

    /* Check target CPU - has capacity check */
    if ((available_idle_cpu(target) || sched_idle_cpu(target)) &&
        asym_fits_capacity(task_util, target))
        return target;

    /* Check prev CPU - has capacity check */
    if (prev != target && cpus_share_cache(prev, target) &&
        (available_idle_cpu(prev) || sched_idle_cpu(prev)) &&
        asym_fits_capacity(task_util, prev))
        return prev;

    /* Per-CPU kthread stacking - MISSING capacity check! */
    if (is_per_cpu_kthread(current) &&
        in_task() &&
        prev == smp_processor_id() &&
        this_rq()->nr_running <= 1) {
        return prev;  /* Returns LITTLE CPU without checking capacity */
    }
    ...
}
```

The function `asym_fits_capacity(task_util, cpu)` checks whether a task's clamped utilization fits within a CPU's capacity:

```c
static inline bool asym_fits_capacity(int task_util, int cpu)
{
    if (static_branch_unlikely(&sched_asym_cpucapacity))
        return fits_capacity(task_util, capacity_of(cpu));
    return true;
}
```

And `fits_capacity()` applies a 20% margin:

```c
static inline bool fits_capacity(unsigned long cap, unsigned long max)
{
    return (cap * 1280) < (max * 1024);
}
```

The `task_util` value is computed by `uclamp_task_util()`, which clamps the task's estimated utilization to the range `[uclamp.min, uclamp.max]`:

```c
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
    return clamp(task_util_est(p),
                 uclamp_eff_value(p, UCLAMP_MIN),
                 uclamp_eff_value(p, UCLAMP_MAX));
}
```

When `uclamp.min` is set high (e.g., 800 on a scale of 0–1024), `uclamp_task_util()` returns at least 800 regardless of the task's actual historical utilization. On a LITTLE CPU with capacity 512, `fits_capacity(800, 512)` evaluates as `800*1280 = 1,024,000 < 512*1024 = 524,288` which is false — the task does not fit. The target and prev checks correctly reject this CPU. But the kthread stacking path returns `prev` (the LITTLE CPU) unconditionally, bypassing this fitness check.

The scenario unfolds as follows:
1. A per-CPU kthread is running on CPU 2 (LITTLE, capacity 512)
2. A CFS task previously ran on CPU 2, then blocked (e.g., waiting for IO)
3. Between activations, the task's `uclamp.min` is raised to 800 (via `sched_setattr()` or cgroup change)
4. The per-CPU kthread wakes the task via `__wake_up_sync()` or similar
5. `select_task_rq_fair()` sets `prev = 2`, `target = 2` (prev CPU is default target)
6. In `select_idle_sibling()`:
   - Target check: `asym_fits_capacity(800, 2)` → false → skip
   - Prev check: `prev == target` → skip (condition `prev != target` is false)
   - Kthread stacking: `is_per_cpu_kthread(current)` → true (kthread bound to one CPU), `prev == smp_processor_id()` → true, `nr_running <= 1` → true → **returns CPU 2**
7. Task is incorrectly placed on the LITTLE CPU despite requiring a BIG CPU

## Consequence

The observable impact is **incorrect task placement on an undersized CPU** on asymmetric CPU capacity systems. A task with a high `uclamp.min` requirement — indicating it needs performance-tier CPU capacity — is placed on a LITTLE/efficiency CPU that cannot provide the required performance.

The practical consequences include:

- **Performance degradation**: The task runs on a CPU with insufficient capacity, causing it to take longer to complete its work. For latency-sensitive tasks (which is typically why `uclamp.min` is set high), this can cause missed deadlines, janky UI rendering, or slow IO completion handling.
- **Defeat of uclamp policy**: System administrators or frameworks (e.g., Android's EAS/uclamp integration) set `uclamp.min` specifically to ensure certain tasks get placed on BIG CPUs. This bug silently ignores that policy, making uclamp unreliable for the per-CPU kthread wakeup pattern, which is extremely common (IO completions, network packet processing, etc.).
- **Regression from intended behavior**: The asymmetric capacity support in `select_idle_sibling()` was explicitly designed to prevent tasks from landing on CPUs that are too small. The kthread stacking path creates a hole in this guarantee.

This bug does not cause crashes, hangs, or data corruption. It is a scheduling quality bug that affects performance and policy enforcement on ARM big.LITTLE systems (and potentially Intel hybrid systems, though x86 hybrid support came after the fix). The impact scales with how frequently per-CPU kthread wakeups occur and how often uclamp values change between task activations.

## Fix Summary

The fix is a one-line addition (plus formatting adjustment) that adds the missing `asym_fits_capacity()` check to the per-CPU kthread stacking path:

```c
/* Before (buggy): */
if (is_per_cpu_kthread(current) &&
    in_task() &&
    prev == smp_processor_id() &&
    this_rq()->nr_running <= 1) {
    return prev;
}

/* After (fixed): */
if (is_per_cpu_kthread(current) &&
    in_task() &&
    prev == smp_processor_id() &&
    this_rq()->nr_running <= 1 &&
    asym_fits_capacity(task_util, prev)) {
    return prev;
}
```

The fix adds `asym_fits_capacity(task_util, prev)` as an additional condition. On symmetric CPU capacity systems (where `sched_asym_cpucapacity` is not set), `asym_fits_capacity()` unconditionally returns `true`, so the added check has zero overhead — the kthread stacking optimization works exactly as before. On asymmetric systems, the check ensures the previous CPU has sufficient capacity for the task's clamped utilization before allowing the fast-path return.

When the capacity check fails (task doesn't fit on the LITTLE CPU), the code falls through to the remaining logic in `select_idle_sibling()`: checking the recently-used CPU (also with capacity check), then entering the asymmetric idle capacity search (`select_idle_capacity()`), which iterates over CPUs in the `sd_asym_cpucapacity` domain to find one where the task fits. This ensures the task lands on an appropriately-sized CPU.

The fix is correct and complete because it aligns the kthread stacking path with every other early-return path in `select_idle_sibling()`, all of which already include `asym_fits_capacity()`. It preserves the kthread stacking optimization for the common case (symmetric systems, or tasks that fit on the current CPU) while closing the hole for the uclamp-induced capacity mismatch case.

## Triggering Conditions

The following precise conditions must all be met simultaneously:

- **Asymmetric CPU capacity system**: The kernel must be running on hardware with different CPU capacities (ARM big.LITTLE, DynamIQ, or Intel Alder Lake/hybrid). The `sched_asym_cpucapacity` static key must be enabled, which requires the scheduler domain hierarchy to include domains with the `SD_ASYM_CPUCAPACITY` flag.

- **CONFIG_UCLAMP_TASK=y**: The kernel must be built with uclamp support. Without uclamp, `uclamp_task_util()` returns the raw task utilization estimate, and the assumption that utilization cannot grow between activations generally holds.

- **Per-CPU kthread as waker**: The task doing the waking must be a per-CPU kthread — specifically, `is_per_cpu_kthread(current)` must return true, which requires `p->flags & PF_KTHREAD` and `p->nr_cpus_allowed == 1`. Common examples include kworkers bound to a single CPU (IO completion workers, network softirq processing kthreads).

- **Kthread on a LITTLE CPU**: The per-CPU kthread must be running on a LITTLE/efficiency CPU whose capacity is smaller than what the wakee task requires. The kthread's CPU is determined by its binding.

- **Wakee previously ran on same CPU**: The wakee task's `prev_cpu` (stored in `p->wake_cpu` or the last CPU it ran on) must equal the kthread's CPU (`smp_processor_id()`). This naturally happens when the wakee queued work to the kthread and then blocked waiting for its completion.

- **High uclamp.min on wakee**: The wakee task must have a `uclamp.min` value high enough that `asym_fits_capacity(uclamp_task_util(p), little_cpu)` returns false. For a LITTLE CPU with capacity 512, `uclamp.min` needs to exceed approximately 410 (since `fits_capacity` uses a 1.25x margin: `410 * 1280 = 524,800 > 512 * 1024 = 524,288`).

- **uclamp.min changed between activations**: The bug is specifically about uclamp.min changing between the task's last execution and its current wakeup. If uclamp.min was already high when the task last ran, the task would already be on a BIG CPU and `prev_cpu` would be a BIG CPU.

- **Low nr_running on kthread's CPU**: The kthread's CPU must have `nr_running <= 1` (only the kthread itself running). This is the "essentially idle" condition for the stacking optimization.

- **Wakeup must go through select_idle_sibling()**: The wakeup must take the CFS task placement path through `select_task_rq_fair()` → `select_idle_sibling()`. This is the normal path for CFS tasks being woken up.

The probability of triggering is moderate on affected systems. The IO completion pattern (per-CPU kworker waking a task) is very common. The uclamp change between activations is the less common factor, but occurs in scenarios like Android's thermal management adjusting uclamp values, or cgroup uclamp policy changes applied to running tasks.

## Reproduce Strategy (kSTEP)

### Why This Bug Cannot Be Reproduced with kSTEP

1. **The bug requires a properly configured asymmetric CPU capacity system.** The `sched_asym_cpucapacity` static key must be enabled, scheduler domains must have `SD_ASYM_CPUCAPACITY` flags, and `per_cpu(sd_asym_cpucapacity, cpu)` pointers must be set to valid `struct sched_domain` entries. These are all set during `build_sched_domains()` based on `arch_scale_cpu_capacity()` returning different values for different CPUs. On x86_64, `arch_scale_cpu_capacity()` returns `SCHED_CAPACITY_SCALE` (1024) for all CPUs on kernels before v6.12, making it impossible to create asymmetric domains.

2. **kSTEP runs on x86_64, and x86 asymmetric capacity support was added in v6.12.** The `kstep_cpu_set_capacity()` function in `kmod/topo.c` has three code paths: (a) `CONFIG_GENERIC_ARCH_TOPOLOGY` (ARM/RISC-V only) — directly sets `per_cpu(cpu_scale, cpu)`, (b) x86 kernel v6.12+ — calls `arch_enable_hybrid_capacity_scale()` and `arch_set_cpu_capacity()`, and (c) x86 kernel < v6.12 — **panics** with "arch_set_cpu_capacity not supported for this kernel". The bug exists in kernel versions v5.10 through v5.16, all of which hit path (c) and panic.

3. **No kernel version exists where both the bug is present AND x86 supports asymmetric capacity.** The bug was fixed in v5.17-rc1 (commit 014ba44e8184). x86 asymmetric CPU capacity support arrived in v6.12 (commit 5a9d10145a54). There is a 5-year gap between the fix and x86 support. On ARM (where asymmetric capacity has always been supported), kSTEP would need to cross-compile and run ARM kernels in QEMU, but the current environment (`uname -m` = x86_64) builds and runs x86 kernels.

4. **Force-enabling the static key alone is insufficient for correct observation.** Even if one were to use `KSYM_IMPORT` to enable `sched_asym_cpucapacity` and directly set `cpu_rq(cpu)->cpu_capacity` to different values, the `per_cpu(sd_asym_cpucapacity, cpu)` pointer would remain NULL because the domain rebuild did not detect capacity asymmetry. Without this pointer, the fixed kernel's fallback path (after correctly rejecting the kthread stacking fast-path) would enter the symmetric LLC-based idle search (`select_idle_cpu()`), which does NOT check capacity. The task would still land on the LITTLE CPU via this fallback, making buggy and fixed kernel behavior **indistinguishable**. Proper reproduction requires the full asymmetric domain infrastructure so that `select_idle_capacity()` is invoked on the fixed kernel to find a BIG CPU.

5. **Maintaining fake capacity values against the scheduler's own updates is fragile.** Even if one manually set domain flags (`SD_ASYM_CPUCAPACITY`) and per-CPU pointers (`sd_asym_cpucapacity`) after a `rebuild_sched_domains()`, the scheduler's `update_cpu_capacity()` (called during every load balance via `update_sd_lb_stats()`) would overwrite `cpu_rq(cpu)->cpu_capacity` based on `arch_scale_cpu_capacity()`, which returns 1024 for all CPUs on pre-v6.12 x86. One would need to continuously re-inject fake capacity values after every load balance tick, creating a brittle and non-deterministic reproducer.

### What Would Need to Be Added to kSTEP

To reproduce this bug, kSTEP would need one of the following:

**Option A: Native aarch64 support.** Run kSTEP on an aarch64 host (or with cross-compilation + QEMU aarch64 emulation). ARM kernels have `CONFIG_GENERIC_ARCH_TOPOLOGY` and `arch_scale_cpu_capacity()` reads from `per_cpu(cpu_scale, cpu)`, which `kstep_cpu_set_capacity()` already sets. Combined with `kstep_topo_apply()` → `rebuild_sched_domains()`, this would create proper asymmetric domains. The existing kSTEP code already supports aarch64 (Makefile and run.py have aarch64 paths), so this would work if the host were ARM or if QEMU aarch64 cross-emulation were set up.

**Option B: Force-enable asymmetric capacity on x86 pre-v6.12.** This would require a substantial kSTEP extension:
1. Use `KSYM_IMPORT` to access `sched_asym_cpucapacity` and call `static_branch_enable()` on it.
2. After `kstep_topo_apply()`, walk all sched_domain structures via `for_each_domain(cpu, sd)`, identify the appropriate domain level (e.g., MC or PKG spanning both BIG and LITTLE CPUs), and set `sd->flags |= SD_ASYM_CPUCAPACITY`.
3. Use `KSYM_IMPORT` to access the `sd_asym_cpucapacity` per-CPU variable and set it to the modified domain for each CPU.
4. Set `cpu_rq(cpu)->cpu_capacity` and `cpu_rq(cpu)->cpu_capacity_orig` to the desired values.
5. Add a callback (e.g., `on_sched_balance_end` or hook `on_tick_begin`) to continuously re-inject the fake capacity values after each load balance overwrites them.
6. Ensure the sched_group_capacity `asym_cpucap` field is set correctly for `find_busiest_group()` to work properly.

This is approximately 60–100 lines of version-specific, fragile code that works against the kernel's own architecture detection. It significantly exceeds the "minor extension" threshold (new helper function, callback hook, sysctl write).

### Alternative Reproduction Methods Outside kSTEP

- **ARM hardware or VM**: Run on actual ARM big.LITTLE hardware (e.g., Odroid, Raspberry Pi with heterogeneous cores, or an ARM cloud instance). Build a v5.16 kernel with `CONFIG_UCLAMP_TASK=y`, create a per-CPU kworker on a LITTLE core, set `uclamp.min` high on a workload task, and trigger IO completions.
- **ftrace/tracepoint approach**: On a stock ARM big.LITTLE system running v5.16, use `trace_sched_wakeup` and `trace_sched_migrate_task` tracepoints to observe a task with high `uclamp.min` being incorrectly placed on a LITTLE CPU after being woken by a per-CPU kthread. Alternatively, add a `kprobe` at the kthread stacking path's `return prev` to log when the fast-path fires despite capacity mismatch.
