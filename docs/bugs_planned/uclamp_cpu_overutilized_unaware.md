# Uclamp: cpu_overutilized() Ignores Uclamp Constraints

**Commit:** `c56ab1b3506ba0e7a872509964b100912bde165d`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.2-rc1
**Buggy since:** v5.3-rc1 (commit `af24bde8df202` "sched/uclamp: Add uclamp support to energy_compute()")

## Bug Description

The `cpu_overutilized()` function in the CFS scheduler determines whether a CPU is overutilized by checking if the CPU's aggregate CFS utilization exceeds a threshold relative to its available capacity. When the system is marked overutilized, Energy Aware Scheduling (EAS) is disabled and the scheduler falls back to traditional load balancing. The function also controls the triggering of misfit migration, where tasks running on CPUs too small for their requirements are migrated to larger CPUs.

Before the fix, `cpu_overutilized()` was completely unaware of utilization clamping (uclamp). It only compared the raw CFS utilization against the CPU capacity using `fits_capacity()`, which applies a 20% migration margin. This caused two major problems on asymmetric CPU capacity systems (big.LITTLE / DynamIQ ARM platforms) when uclamp was in use:

**Problem 1 — False overutilization from UCLAMP_MAX-capped tasks:** When a busy task is capped with `UCLAMP_MAX` to a value that fits a CPU's original capacity, the system should NOT be considered overutilized on that CPU. For example, a task with `UCLAMP_MAX = 512` running on a CPU with `capacity_orig = 512` should fit perfectly — the cap tells the scheduler this task doesn't need more capacity. But the buggy code only checks the raw CFS utilization, which can reach the CPU's full capacity regardless of the clamp. Since `fits_capacity()` includes a 20% margin (`util * 1.25 < capacity`), a fully-utilized CPU is always "overutilized," even though the uclamp cap guarantees the task doesn't need a bigger CPU. On Google Pixel 6, this caused the system to be in overutilized state 74.5% of the time during JIT compilation workloads (a common background activity), disabling EAS and wasting significant energy. With the fix, overutilized time dropped to 9.79%.

**Problem 2 — Missing overutilization from UCLAMP_MIN-boosted tasks:** When a task has its `UCLAMP_MIN` increased while running (e.g., a background task promoted to interactive priority), it may need to migrate to a larger CPU to honor the new minimum performance requirement. The overutilized flag must be set to trigger misfit migration. But the buggy code only checks the task's actual CFS utilization, which may be low if the task is lightweight. Since the raw utilization fits the small CPU, overutilized is never set, and misfit migration never triggers at tick time. The task stays stuck on the small CPU until its next wakeup, violating the uclamp_min guarantee.

## Root Cause

The root cause is that `cpu_overutilized()` used only `fits_capacity()` for its check, which is a simple capacity comparison with a 20% migration margin, completely ignorant of uclamp constraints:

```c
// BUGGY (before fix):
static inline bool cpu_overutilized(int cpu)
{
    return !fits_capacity(cpu_util_cfs(cpu), capacity_of(cpu));
}
```

Where `fits_capacity()` is defined as:
```c
#define fits_capacity(cap, max)  ((cap) * 1280 < (max) * 1024)
```

This checks whether `cap < 0.8 * max` — i.e., the utilization is within 80% of the available capacity. The 20% headroom is the "migration margin" designed to trigger upmigration before a CPU is fully saturated.

The problem is that this check makes no distinction between a task whose actual computational demand exceeds the CPU capacity and a task that is merely capped by uclamp to a level that fits the CPU. Consider a CPU with `capacity_orig = 512` and a task capped at `UCLAMP_MAX = 512`:

1. The task runs and accumulates PELT utilization. `cpu_util_cfs(cpu)` converges toward `capacity_orig_of(cpu) = 512` (since `cpu_util_cfs()` is capped at `capacity_orig_of(cpu)` via `min(util, capacity_orig_of(cpu))`).
2. `capacity_of(cpu)` equals approximately 512 (the available capacity after subtracting any IRQ/thermal pressure).
3. `fits_capacity(512, 512)` evaluates to `512 * 1280 < 512 * 1024` → `655360 < 524288` → **false**.
4. Therefore `cpu_overutilized()` returns **true**, marking the CPU (and thus the root domain) as overutilized.

This is incorrect because the task's uclamp_max cap of 512 means it only requires a CPU with at least 512 capacity — which is exactly what this CPU provides. The overutilized flag should not be set.

Conversely, for the UCLAMP_MIN problem: a task with low actual utilization (e.g., `cpu_util_cfs(cpu) = 100`) on a CPU with `capacity_of(cpu) = 512`:

1. `fits_capacity(100, 512)` evaluates to `100 * 1280 < 512 * 1024` → `128000 < 524288` → **true**.
2. `cpu_overutilized()` returns **false** — the CPU is not overutilized.

But if this task has `UCLAMP_MIN = 1024`, it requires a CPU with capacity 1024 to meet its minimum performance guarantee. The CPU with capacity 512 cannot satisfy this. The overutilized flag should be set to enable misfit migration, but the buggy code never considers the uclamp_min requirement.

The fix replaces `fits_capacity()` with `util_fits_cpu()` (introduced in the same patch series as patch 1/9), which properly accounts for uclamp constraints by comparing `uclamp_max` against `capacity_orig_of(cpu)` (without the migration margin or pressure deductions) and checking whether `uclamp_min` can be satisfied by the CPU's thermal-adjusted original capacity.

## Consequence

The most significant observable consequence is excessive energy consumption on asymmetric CPU capacity systems (ARM big.LITTLE / DynamIQ). When UCLAMP_MAX-capped tasks falsely trigger the overutilized state:

1. **EAS is disabled**: `find_energy_efficient_cpu()` exits early when `rd->overutilized` is set (line 7192: `if (!pd || READ_ONCE(rd->overutilized))`). Without EAS, the scheduler cannot make energy-aware placement decisions and falls back to traditional load balancing, which distributes tasks for throughput without considering power consumption. On Pixel 6, this caused the system to remain overutilized 74.5% of the time during JIT compilation — a common Android background workload — compared to only 9.79% with the fix.

2. **Misfit migration fails for boosted tasks**: When a task's `UCLAMP_MIN` is raised while it is running on a small CPU, the task should be migrated to a larger CPU via misfit migration. The misfit migration mechanism depends on the overutilized flag being set (either via `update_overutilized_status()` during enqueue/tick, or via `update_sg_wakeup_stats()` during load balancing). Since the buggy `cpu_overutilized()` only checks raw utilization, a lightweight task with a high `UCLAMP_MIN` boost does not trigger overutilization. This means `update_misfit_status()` is called but the broader overutilized check that gates certain load balancing behaviors is missing. The task remains stuck on the small CPU until it sleeps and wakes up, during which `select_task_rq_fair()` or `find_energy_efficient_cpu()` may place it on a bigger CPU. For long-running tasks that don't sleep frequently, this results in sustained violation of the uclamp_min performance guarantee.

3. **No crash or data corruption**: The bug is strictly a scheduling quality regression. There are no kernel panics, NULL pointer dereferences, or lockups. The system remains functional but makes suboptimal scheduling decisions, leading to measurable energy waste and performance degradation on affected platforms.

## Fix Summary

The fix modifies `cpu_overutilized()` to use the new `util_fits_cpu()` function instead of the raw `fits_capacity()` check. The `util_fits_cpu()` function was introduced in patch 1/9 of the same series ("sched/uclamp: Fix relationship between uclamp and migration margin") and encapsulates the logic for correctly comparing utilization against CPU capacity when uclamp is active.

The change is minimal — only the `cpu_overutilized()` function body changes:

```c
// FIXED:
static inline bool cpu_overutilized(int cpu)
{
    unsigned long rq_util_min = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MIN);
    unsigned long rq_util_max = uclamp_rq_get(cpu_rq(cpu), UCLAMP_MAX);

    return !util_fits_cpu(cpu_util_cfs(cpu), rq_util_min, rq_util_max, cpu);
}
```

The function now retrieves the per-runqueue aggregated uclamp values via `uclamp_rq_get()` (which returns the maximum `uclamp_min` and maximum `uclamp_max` across all tasks currently enqueued on the runqueue) and passes them to `util_fits_cpu()`.

Inside `util_fits_cpu()`, the key logic for UCLAMP_MAX is:
```c
uclamp_max_fits = (capacity_orig == SCHED_CAPACITY_SCALE) && (uclamp_max == SCHED_CAPACITY_SCALE);
uclamp_max_fits = !uclamp_max_fits && (uclamp_max <= capacity_orig);
fits = fits || uclamp_max_fits;
```
This says: if `uclamp_max <= capacity_orig_of(cpu)` (and we're not on the max-capacity CPU with max clamp — a special case to avoid blocking overutilized on the biggest CPU), then the task fits regardless of raw utilization. This comparison uses `capacity_orig` (not `capacity_of`), ignoring both the migration margin and transient capacity pressure, because the uclamp cap is a hard policy constraint, not a dynamic utilization signal.

For UCLAMP_MIN, the logic at the end of `util_fits_cpu()` is:
```c
uclamp_min = min(uclamp_min, uclamp_max);
if (util < uclamp_min && capacity_orig != SCHED_CAPACITY_SCALE)
    fits = fits && (uclamp_min <= capacity_orig_thermal);
```
This says: if the task is boosted (its actual util is below `uclamp_min`) and we're not on the max-capacity CPU, then the task only fits if the boost value fits within the CPU's thermally-adjusted original capacity. If `uclamp_min` exceeds `capacity_orig_thermal`, the task doesn't fit and the CPU is overutilized.

When `uclamp_is_used()` returns false (no uclamp configured on the system), `util_fits_cpu()` falls back to `fits_capacity()`, preserving the original behavior for systems without uclamp.

## Triggering Conditions

The following conditions must be met to trigger the bug:

- **Asymmetric CPU capacity**: The system must have CPUs with different original capacities (e.g., `capacity_orig_of(cpu0) = 512`, `capacity_orig_of(cpu1) = 1024`). On symmetric systems, while the overutilized check still runs, the uclamp-specific corrections in `util_fits_cpu()` are less impactful because all CPUs have `capacity_orig == SCHED_CAPACITY_SCALE`.

- **CONFIG_UCLAMP_TASK=y**: Utilization clamping must be compiled in and active. The `uclamp_is_used()` static key must be enabled, which happens automatically when any task or cgroup sets a uclamp value via `sched_setattr()` or cgroup `cpu.uclamp.{min,max}` writes.

- **CONFIG_SMP=y**: The `cpu_overutilized()` function and `update_overutilized_status()` are only compiled under `CONFIG_SMP`.

- **For Problem 1 (false overutilization with UCLAMP_MAX):**
  - A CFS task must have `UCLAMP_MAX` set to a value ≤ `capacity_orig_of(target_cpu)`.
  - The task must run long enough for its PELT `util_avg` to exceed 80% of `capacity_of(target_cpu)` (the `fits_capacity()` migration margin threshold). Since `cpu_util_cfs()` is capped at `capacity_orig_of(cpu)`, a fully-utilized task on any CPU will always exceed this threshold.
  - The task must be enqueued (triggering `update_overutilized_status()` from `enqueue_task_fair()`) or the scheduler tick must fire (triggering `update_overutilized_status()` from `task_tick_fair()`).
  - At least 2 CPUs so `CONFIG_SMP` paths are active.
  - No race conditions involved — the bug is fully deterministic.

- **For Problem 2 (missing overutilization with UCLAMP_MIN):**
  - A CFS task must have `UCLAMP_MIN` set to a value > `capacity_orig_of(target_cpu)`.
  - The task's actual CFS utilization (`cpu_util_cfs()`) must be low enough that `fits_capacity(util, capacity)` returns true (i.e., `util < 0.8 * capacity`).
  - The task is running on a small CPU that cannot satisfy the `UCLAMP_MIN` requirement.
  - No race conditions involved — the bug is fully deterministic.

- **Timing**: The bug manifests continuously once conditions are met. Every tick and every enqueue operation re-evaluates `cpu_overutilized()` with the same buggy logic. Reproduction reliability is 100% once PELT has converged.

## Reproduce Strategy (kSTEP)

This bug is reproducible with kSTEP. The overutilized flag is computed and stored regardless of whether EAS/Energy Model are active — it is part of the standard SMP scheduling path. kSTEP provides all necessary primitives: asymmetric CPU capacities, uclamp configuration, tick control, and direct access to internal scheduler state.

### Setup

1. **QEMU Configuration**: 2 CPUs minimum. CPU 0 is reserved for the driver, so CPU 1 will be the test CPU.

2. **Asymmetric Capacity**: Use `kstep_cpu_set_capacity(1, 512)` to set CPU 1's original capacity to 512 (half of `SCHED_CAPACITY_SCALE = 1024`). Also call `kstep_cpu_set_freq(1, 512)` so that PELT frequency-invariant accounting reflects the lower capacity. This ensures `capacity_orig_of(1) = 512` and `capacity_of(1) ≈ 512`.

3. **Kernel Version Guard**: The buggy code exists from v5.3-rc1 through v6.1.x. The fix landed in v6.2-rc1. Guard the driver with `#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)`.

### Scenario 1: False Overutilization with UCLAMP_MAX (Primary Bug)

This demonstrates that a UCLAMP_MAX-capped task incorrectly triggers overutilized state on the buggy kernel.

**Step-by-step plan:**

1. **Create a CFS task**: `struct task_struct *p = kstep_task_create()`.

2. **Set UCLAMP_MAX = 512** on the task using `sched_setattr_nocheck()`:
   ```c
   struct sched_attr attr = {
       .size = sizeof(attr),
       .sched_policy = SCHED_NORMAL,
       .sched_flags = SCHED_FLAG_UTIL_CLAMP,
       .sched_util_min = 0,
       .sched_util_max = 512,
   };
   sched_setattr_nocheck(p, &attr);
   ```

3. **Pin the task to CPU 1**: `kstep_task_pin(p, 1, 2)` (CPUs 1 inclusive, 2 exclusive).

4. **Wake the task**: `kstep_task_wakeup(p)`.

5. **Run ticks to build PELT**: `kstep_tick_repeat(200)` — 200 ticks should be sufficient for PELT `util_avg` to converge toward the CPU's full capacity. (PELT has a half-life of ~32ms ≈ 32 ticks at 1ms tick, so 200 ticks provides ~6 half-lives for near-full convergence.)

6. **Read the overutilized flag**:
   ```c
   struct rq *rq = cpu_rq(1);
   int overutilized = READ_ONCE(rq->rd->overutilized);
   unsigned long util = cpu_util_cfs(1);
   ```

7. **Pass/fail criteria**:
   - Import `cpu_util_cfs` via `KSYM_IMPORT(cpu_util_cfs)` or read `cpu_rq(1)->cfs.avg.util_avg` directly.
   - Verify `util > 0` (PELT has accumulated).
   - On **buggy kernel**: `overutilized == SG_OVERUTILIZED` (value 0x2). Report `kstep_fail("BUG: overutilized=%d with UCLAMP_MAX=%d on cap=%d CPU", overutilized, 512, 512)`.
   - On **fixed kernel**: `overutilized == 0`. Report `kstep_pass("FIXED: overutilized=%d, uclamp_max correctly prevents false overutilization", overutilized)`.

### Scenario 2: Missing Overutilization with UCLAMP_MIN (Secondary Bug)

This demonstrates that a UCLAMP_MIN-boosted task on a small CPU fails to trigger overutilized state on the buggy kernel.

**Step-by-step plan:**

1. **Create a CFS task**: `struct task_struct *p = kstep_task_create()`.

2. **Set UCLAMP_MIN = 1024** on the task:
   ```c
   struct sched_attr attr = {
       .size = sizeof(attr),
       .sched_policy = SCHED_NORMAL,
       .sched_flags = SCHED_FLAG_UTIL_CLAMP,
       .sched_util_min = 1024,
       .sched_util_max = 1024,
   };
   sched_setattr_nocheck(p, &attr);
   ```

3. **Pin the task to CPU 1** (cap=512): `kstep_task_pin(p, 1, 2)`.

4. **Wake the task and run a few ticks**: `kstep_task_wakeup(p)` followed by `kstep_tick_repeat(10)`. We run only a few ticks so that `cpu_util_cfs(1)` is still low (below 80% of 512 ≈ 410). The task's actual utilization is small, but its `UCLAMP_MIN` demands a larger CPU.

5. **Read the overutilized flag**:
   ```c
   struct rq *rq = cpu_rq(1);
   int overutilized = READ_ONCE(rq->rd->overutilized);
   unsigned long util = cpu_util_cfs(1);
   unsigned long rq_uclamp_min = uclamp_rq_get(cpu_rq(1), UCLAMP_MIN);
   ```

6. **Pass/fail criteria**:
   - Verify `util < 410` (utilization hasn't converged yet — still below the `fits_capacity` threshold).
   - Verify `rq_uclamp_min == 1024` (uclamp_min is correctly propagated to the RQ).
   - On **buggy kernel**: `overutilized == 0`. Since `fits_capacity(util, 512)` returns true for low util, the CPU is not considered overutilized, even though the uclamp_min demand of 1024 exceeds the CPU's capacity of 512. Report `kstep_fail("BUG: overutilized=%d, uclamp_min=%lu on cap=512 CPU but no overutilized", overutilized, rq_uclamp_min)`.
   - On **fixed kernel**: `overutilized == SG_OVERUTILIZED`. The `util_fits_cpu()` function detects that `uclamp_min(1024) > capacity_orig_thermal(512)` when `util < uclamp_min` and `capacity_orig != SCHED_CAPACITY_SCALE`, so the task doesn't fit, triggering overutilized. Report `kstep_pass("FIXED: overutilized=%d, uclamp_min=%lu correctly triggers overutilized on small CPU", overutilized, rq_uclamp_min)`.

### Using the `on_tick_begin` Callback for Detailed Tracing

Use the `on_tick_begin` callback to log per-tick state for debugging:

```c
static void on_tick_begin(void) {
    struct rq *rq = cpu_rq(1);
    unsigned long util = rq->cfs.avg.util_avg;
    unsigned long util_est = rq->cfs.avg.util_est.enqueued;
    int overutilized = READ_ONCE(rq->rd->overutilized);
    unsigned long cap = capacity_of(1);
    unsigned long cap_orig = capacity_orig_of(1);

    kstep_json_print_2kv("type", "tick_state",
        "util", "%lu", util);
    kstep_json_print_2kv("type", "tick_overutil",
        "overutilized", "%d", overutilized);
}
```

This provides visibility into PELT convergence and the exact tick at which overutilized transitions, helping verify determinism across runs.

### Notes on kSTEP Compatibility

- **No kSTEP framework changes needed**: All required APIs exist — `kstep_cpu_set_capacity()`, `kstep_cpu_set_freq()`, `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()`, `kstep_tick_repeat()`. The uclamp configuration uses `sched_setattr_nocheck()` which is a standard kernel API already used in the existing `uclamp_inversion.c` driver.
- **Internal state access**: `cpu_rq()`, `rd->overutilized`, `cfs_rq->avg.util_avg`, `capacity_of()`, `capacity_orig_of()`, `uclamp_rq_get()` are all accessible via `internal.h`.
- **No EAS/energy model required**: The overutilized flag is computed in the standard SMP CFS enqueue and tick paths, independent of EAS. We are directly observing the `cpu_overutilized()` result via the `rd->overutilized` flag.
- **Deterministic**: Both scenarios are fully deterministic — no race conditions, no timing sensitivity. The bug manifests on every tick/enqueue once PELT has accumulated.
