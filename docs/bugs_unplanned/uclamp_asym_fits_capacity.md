# Uclamp: asym_fits_capacity() Ignores Migration Margin and Capacity Pressure Interaction

**Commit:** `a2e7f03ed28fce26c78b985f87913b6ce3accf9d`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.2-rc1
**Buggy since:** v5.10-rc4 (introduced by commit `b4c9c9f15649` "sched/fair: Prefer prev cpu in asymmetric wakeup path")

## Bug Description

On asymmetric CPU capacity systems (e.g., ARM big.LITTLE), the `asym_fits_capacity()` function in `select_idle_sibling()` incorrectly determines whether a task fits a given CPU when uclamp (utilization clamping) is active. The function applies the `fits_capacity()` macro, which includes a 20% migration margin, directly to the uclamp-adjusted task utilization. This causes tasks with high `uclamp_min` values (boosted tasks) to be rejected from CPUs they should fit on, and tasks with low `uclamp_max` values (capped tasks) to be pushed to unnecessarily large CPUs.

The bug is part of a broader issue identified by Qais Yousef in the patch series "[PATCH v2 0/9] Fix relationship between uclamp and fits_capacity()". The core problem is that `fits_capacity()` was designed for raw utilization values, not for uclamp bounds. When uclamp bounds are passed through `fits_capacity()`, the 20% migration margin creates incorrect fitness decisions. For example, a task boosted to `SCHED_CAPACITY_SCALE` (1024) via `uclamp_min` will NEVER pass `fits_capacity()` on any CPU, because `fits_capacity(1024, 1024)` evaluates to `(1024 * 1280 < 1024 * 1024)` which is `(1310720 < 1048576)` = false. Similarly, a task capped to the exact capacity of a medium CPU will be rejected from that CPU due to the margin.

The `asym_fits_capacity()` function also ignores capacity pressure (IRQ pressure, thermal throttling) and their distinct impacts on uclamp_min vs uclamp_max decisions. Capacity pressure should reduce the effective capacity when evaluating raw utilization, but uclamp bounds should be compared against the original CPU capacity (without pressure), since a slight transient pressure shouldn't invalidate a user-specified performance request.

## Root Cause

The root cause is in the `asym_fits_capacity()` function and its caller `select_idle_sibling()`:

```c
static inline bool asym_fits_capacity(unsigned long task_util, int cpu)
{
    if (sched_asym_cpucap_active())
        return fits_capacity(task_util, capacity_of(cpu));
    return true;
}
```

This function is called at several points in `select_idle_sibling()` to determine if a task can be placed on a candidate CPU (target, prev, or recent_used_cpu). The `task_util` parameter is computed as:

```c
task_util = uclamp_task_util(p);
```

where `uclamp_task_util()` returns `clamp(task_util_est(p), uclamp_min, uclamp_max)`. This means the utilization value passed to `fits_capacity()` has already been clamped by uclamp bounds.

The `fits_capacity()` macro is defined as:

```c
#define fits_capacity(cap, max) ((cap) * 1280 < (max) * 1024)
```

This applies a ~20% headroom margin to speed up upmigration. For raw utilization signals, this margin makes sense — if a task's utilization is within 80% of the CPU capacity, it's time to migrate up. But for uclamp bounds, this margin is inappropriate:

1. **Boosted tasks (high uclamp_min):** A task with `uclamp_min = 1024` gets `task_util = 1024`. Then `fits_capacity(1024, 1024)` = false. The task doesn't "fit" even the largest CPU, which is nonsensical — the user explicitly requested maximum performance.

2. **Capped tasks (low uclamp_max):** A task with `uclamp_max = capacity_orig_of(medium_cpu)` gets `task_util = capacity_orig_of(medium_cpu)`. Then `fits_capacity(medium_cap, medium_cap)` = false. The task is pushed to a big CPU despite the user explicitly capping it to the medium CPU's performance level.

3. **Capacity pressure ignored for uclamp bounds:** The function uses `capacity_of(cpu)` which subtracts IRQ pressure. A tiny amount of IRQ pressure on a big CPU makes `capacity_of(big_cpu) < capacity_orig_of(big_cpu)`, further exacerbating the margin problem for boosted tasks.

The correct approach, implemented by the fix, is to separate the raw utilization check from the uclamp bounds check. The raw utilization should be checked with `fits_capacity()` (including margin and pressure), while uclamp bounds should be compared directly against `capacity_orig_of()` (without margin, using only thermal pressure for `uclamp_min`).

## Consequence

The primary consequence is **incorrect CPU selection during task wakeup on asymmetric CPU capacity systems with uclamp**. Specifically:

1. **Boosted tasks cannot be placed on any CPU via the fast path.** Since `fits_capacity(1024, 1024)` is always false, `select_idle_sibling()` never returns early for maximally-boosted tasks. This forces the scheduler through the slower `select_idle_capacity()` path or the LLC-wide idle CPU scan, increasing wakeup latency. On some topologies, the task may be placed on a suboptimal CPU because the asymmetric capacity domain scan produces different results than the fast-path checks.

2. **Capped tasks are unnecessarily upmigrated.** A task capped to a medium CPU's capacity is rejected from that CPU and migrated to a big CPU. This wastes power on battery-constrained mobile/embedded devices (the primary users of uclamp) and defeats the purpose of the UCLAMP_MAX cap. It can also cause task ping-ponging between CPUs, as the load balancer tries to rebalance tasks that shouldn't have been migrated.

3. **Scheduling instability.** Because the fast-path checks in `select_idle_sibling()` fail for uclamp-affected tasks, these tasks always go through full idle CPU scans. Combined with the incorrect capacity-fitting decisions, this can lead to repeated unnecessary migrations, increased context switches, and degraded application performance. The cover letter of the patch series notes that this affects "sched pipe" benchmarks by up to 22% on heterogeneous ARM platforms.

## Fix Summary

The fix replaces `asym_fits_capacity()` with a new `asym_fits_cpu()` function that uses the `util_fits_cpu()` helper (introduced in patch 1/9 of the same series):

```c
static inline bool asym_fits_cpu(unsigned long util,
                                 unsigned long util_min,
                                 unsigned long util_max,
                                 int cpu)
{
    if (sched_asym_cpucap_active())
        return util_fits_cpu(util, util_min, util_max, cpu);
    return true;
}
```

The `util_fits_cpu()` function properly handles the interaction between uclamp and capacity checking by:
- Checking raw utilization (`util`) against `capacity_of(cpu)` with the migration margin (via `fits_capacity()`) — this preserves the upmigration acceleration behavior for actual utilization.
- Checking `uclamp_max` against `capacity_orig_of(cpu)` **without** migration margin — if the user cap fits the CPU's original capacity, the task fits.
- Checking `uclamp_min` against `capacity_orig_thermal` (capacity_orig minus thermal pressure) **without** migration margin — a boosted task fits if its boost target is achievable given thermal constraints.
- Special-casing the maximum capacity CPU to avoid blocking overutilized detection when both `capacity_orig` and `uclamp_max` equal `SCHED_CAPACITY_SCALE`.

The fix also changes how `task_util` is computed in `select_idle_sibling()`: instead of using `uclamp_task_util(p)` (which returns the clamped value), it now uses `task_util_est(p)` for the raw utilization and separately passes `uclamp_eff_value(p, UCLAMP_MIN)` and `uclamp_eff_value(p, UCLAMP_MAX)` to `asym_fits_cpu()`. This clean separation ensures that the raw utilization and uclamp bounds are evaluated independently with appropriate semantics.

## Triggering Conditions

To trigger this bug, the following conditions must ALL be met:

1. **Asymmetric CPU capacity system:** `sched_asym_cpucap_active()` must return true. This requires CPUs with different `capacity_orig_of()` values and sched domains built with `SD_ASYM_CPUCAPACITY`. In practice, this means ARM big.LITTLE or DynamIQ systems, or Intel hybrid systems with capacity differentiation.

2. **CONFIG_UCLAMP_TASK=y:** The kernel must be built with uclamp support enabled.

3. **Active uclamp bounds on the task:** The task must have non-default `uclamp_min` or `uclamp_max` values. The bug is most pronounced with `uclamp_min = SCHED_CAPACITY_SCALE` (1024) or `uclamp_max = capacity_orig_of(medium_cpu)`.

4. **Task wakeup triggering `select_idle_sibling()`:** The CFS wakeup path must enter `select_idle_sibling()`, which happens during `select_task_rq_fair()` when `WF_TTWU` is set and the wakeup is not a sync wakeup to a single-CPU task.

5. **Candidate CPUs are idle:** The target, prev, or recent_used_cpu must be idle (or sched-idle), otherwise the `available_idle_cpu()` check would reject them before `asym_fits_capacity()` is even evaluated.

The bug is **deterministic** given these conditions — there is no race condition or timing sensitivity. Every single wakeup of a uclamp-boosted task on an asymmetric system hits the buggy code path. The probability of reproduction is 100% on affected hardware configurations.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. Here is the detailed explanation:

### 1. WHY can this bug not be reproduced with kSTEP?

The bug requires **asymmetric CPU capacity** — CPUs with different `capacity_orig_of()` values — and `sched_asym_cpucap_active()` returning true. kSTEP runs in QEMU on x86_64, and on x86_64 kernels in the buggy range (v5.15 to v6.1), the `capacity_orig_of()` function is a **compile-time constant** that always returns `SCHED_CAPACITY_SCALE` (1024):

```c
// On x86 without CONFIG_GENERIC_ARCH_TOPOLOGY:
static inline unsigned long capacity_orig_of(int cpu)
{
    return SCHED_CAPACITY_SCALE;  // always 1024
}
```

`CONFIG_GENERIC_ARCH_TOPOLOGY` is not available on x86_64 for kernels before v6.12. Without it, there is no per-CPU `cpu_scale` variable to manipulate — the capacity value is hardcoded by the compiler. Consequently:

- `kstep_cpu_set_capacity()` **panics** on x86 pre-6.12 kernels (the function's `#else` branch calls `panic()`).
- Even if the panic were bypassed, there is no runtime mechanism to make `capacity_orig_of()` return different values on different CPUs.
- Without actual capacity asymmetry, `sched_asym_cpucap_active()` remains false, and the buggy `asym_fits_capacity()` code path is never entered (it unconditionally returns `true`).

### 2. WHAT would need to be added to kSTEP to support this?

Reproducing this bug would require one of the following fundamental changes:

**Option A: ARM/aarch64 kernel support.** kSTEP would need to cross-compile and run ARM kernels in QEMU aarch64 emulation. ARM kernels have `CONFIG_GENERIC_ARCH_TOPOLOGY` enabled by default, so `kstep_cpu_set_capacity()` works and `capacity_orig_of()` returns per-CPU values. This is a major infrastructure change (QEMU aarch64 configuration, cross-compilation toolchain, architecture-specific kernel config).

**Option B: x86 hybrid capacity support (kernel ≥ 6.12).** kSTEP's `kstep_cpu_set_capacity()` already supports x86 kernels ≥ 6.12 via `arch_enable_hybrid_capacity_scale()`. However, the bug is fixed in v6.2-rc1, so by the time x86 can set asymmetric capacities (v6.12), the bug no longer exists. This option is a temporal impossibility.

**Option C: Kernel source patching.** kSTEP could patch the kernel source to enable `CONFIG_GENERIC_ARCH_TOPOLOGY` on x86 for older kernels, or replace the inline `capacity_orig_of()` with a per-CPU variable lookup. This is fragile and architecture-intrusive, and may introduce unintended side effects in the scheduler's topology code.

None of these options constitute a "minor extension" to kSTEP — they all require fundamental changes to the build infrastructure, architecture support, or kernel source modification.

### 3. Alternative reproduction methods

- **ARM hardware:** Run the buggy kernel (e.g., v6.1) on an ARM big.LITTLE board (e.g., Raspberry Pi 4 with different core types, or Odroid-XU4 with Exynos 5422). Create a task with `uclamp_min = 1024` using `sched_setattr()` and observe CPU placement during wakeups. The task should be incorrectly rejected from fitting CPUs on the buggy kernel.

- **ARM QEMU emulation (outside kSTEP):** Use QEMU aarch64 with a custom device tree defining CPUs with different capacities. Boot a v6.1 kernel and run a test program that sets uclamp bounds and monitors task migration via `/proc/[pid]/stat` or `sched:sched_wakeup` tracepoints.

- **Intel hybrid hardware (limited):** On Intel Alder Lake/Raptor Lake with kernel v6.0-v6.1 (which has basic hybrid capacity support via HFI), the bug could potentially be triggered. However, HFI-based capacity setting requires real hardware and cannot be emulated in QEMU.

### 4. Detection methodology (if the environment supported it)

If asymmetric capacity were available, the driver would:
1. Set up 2+ CPUs with different capacities (e.g., CPU1=1024, CPU2=512)
2. Create a task with `uclamp_min = 512` (boosted to medium CPU capacity)
3. Pin the task to CPU2 (medium), let it run with low actual utilization (~100)
4. Unpin, block, then wake the task while CPU2 is idle
5. On the **buggy** kernel: `asym_fits_capacity(512, cpu2)` → `fits_capacity(512, 512)` → false. CPU2 is rejected despite being the correct choice. Task migrates to CPU1 (big).
6. On the **fixed** kernel: `asym_fits_cpu(100, 512, 1024, cpu2)` → `util_fits_cpu()` correctly evaluates that raw util 100 fits CPU2's capacity, and `uclamp_min=512 ≤ capacity_orig_of(cpu2)=512`. Task stays on CPU2.
7. Check `task_cpu(p)` after wakeup: CPU2 = pass, CPU1 = fail.
