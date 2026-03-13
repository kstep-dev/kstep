# Uclamp: Stale Max Aggregation on Idle Runqueue

**Commit:** `3e1493f46390618ea78607cb30c58fc19e2a5035`
**Affected files:** kernel/sched/sched.h
**Fixed in:** v5.14-rc1
**Buggy since:** v5.3-rc1 (introduced by commit 9d20ad7dfc9a "sched/uclamp: Add uclamp_util_with()")

## Bug Description

The utilization clamping (uclamp) subsystem allows userspace to set per-task minimum and maximum utilization bounds via `sched_setattr()`. These per-task clamps are aggregated at the runqueue (rq) level using MAX aggregation: `rq->uclamp[UCLAMP_MIN].value` holds the maximum of all runnable tasks' UCLAMP_MIN values, and `rq->uclamp[UCLAMP_MAX].value` holds the maximum of all runnable tasks' UCLAMP_MAX values. These aggregated values are used throughout the scheduler for CPU frequency selection (via schedutil), energy-aware scheduling (EAS) placement decisions, and capacity fitting checks.

The function `uclamp_rq_util_with()` in `kernel/sched/sched.h` is the primary interface for computing a utilization value clamped by uclamp bounds. When called with both a runqueue `rq` and a task `p`, it computes the effective clamp as `max(rq->uclamp[clamp_id].value, uclamp_eff_value(p, clamp_id))`. This max aggregation is correct when the rq is actively running tasks, because the rq-level clamp reflects the aggregate of all currently runnable tasks. However, when the rq is idle, the rq-level uclamp values are stale — they reflect the last task that was dequeued, not any currently runnable task.

When a task wakes up and is being considered for placement on an idle CPU (e.g., during `find_energy_efficient_cpu()`), `uclamp_rq_util_with()` is called *before* the task is actually enqueued. At this point the rq still has `UCLAMP_FLAG_IDLE` set and the rq's uclamp values are stale. The function incorrectly max-aggregates the task's clamp values with these stale rq values, producing incorrect results. This is particularly problematic for `UCLAMP_MAX` because the stale rq value is typically 1024 (the default max), which effectively nullifies any lower uclamp_max the waking task might have.

This bug primarily manifests during EAS task placement in `find_energy_efficient_cpu()`, where `uclamp_rq_util_with()` is called to determine whether a CPU can fit a task (the `fits_capacity()` check) and to estimate the energy cost of placing the task on each candidate CPU. With inflated uclamp_max values, the scheduler may make suboptimal placement decisions — for example, not capping the frequency on a CPU where the task's uclamp_max should have limited it, or incorrectly computing spare capacity.

## Root Cause

The root cause is in `uclamp_rq_util_with()` in `kernel/sched/sched.h`. Before the fix, the function unconditionally reads the rq-level uclamp values and max-aggregates them with the task's effective uclamp values:

```c
min_util = READ_ONCE(rq->uclamp[UCLAMP_MIN].value);
max_util = READ_ONCE(rq->uclamp[UCLAMP_MAX].value);

if (p) {
    min_util = max(min_util, uclamp_eff_value(p, UCLAMP_MIN));
    max_util = max(max_util, uclamp_eff_value(p, UCLAMP_MAX));
}
```

The problem is that `rq->uclamp[UCLAMP_MAX].value` on an idle rq is not zero or undefined — it retains the value from the last runnable task. This is by design: when the last task is dequeued from a CPU, `uclamp_rq_dec_id()` calls `uclamp_idle_value()`, which for `UCLAMP_MAX` sets `UCLAMP_FLAG_IDLE` and *retains* the last max-clamp value. This retention exists to prevent blocked utilization from pushing up CPU frequency after the CPU goes idle (the old max-clamp is kept so schedutil does not suddenly request a lower frequency while PELT decay is still pending).

The lifecycle is: (1) last task dequeues from rq → `uclamp_rq_dec_id()` → `uclamp_idle_value()` sets `UCLAMP_FLAG_IDLE` and retains `rq->uclamp[UCLAMP_MAX].value` at the old task's level; (2) new task wakes up → `find_energy_efficient_cpu()` evaluates this rq → calls `uclamp_rq_util_with(cpu_rq(cpu), util, p)` → reads the stale `rq->uclamp[UCLAMP_MAX].value` (e.g., 1024) → computes `max(1024, p->uclamp_max)` → always returns 1024 regardless of the new task's lower clamp; (3) only when the task is actually enqueued does `uclamp_rq_inc()` clear `UCLAMP_FLAG_IDLE` and `uclamp_idle_reset()` overwrite the rq value with the new task's clamp.

The key insight is that between steps (2) and (3), the rq's uclamp values are meaningless for the purpose of evaluating the *upcoming* task — they represent a previous task that is no longer runnable on this rq. The max aggregation with these stale values produces an inflated result, defeating the purpose of the task's own `uclamp_max` cap.

Consider a concrete example: Task A with `uclamp_max = 1024` (default) runs on CPU 1 and then sleeps. CPU 1 goes idle with `rq->uclamp[UCLAMP_MAX].value = 1024` and `UCLAMP_FLAG_IDLE` set. Task B with `uclamp_max = 512` wakes up and the scheduler evaluates CPU 1 as a candidate. `uclamp_rq_util_with()` computes `max(1024, 512) = 1024`, completely ignoring Task B's cap of 512. The scheduler then believes CPU 1 can utilize up to 1024 for Task B, which is wrong — the task should be capped at 512.

## Consequence

The primary consequence is incorrect CPU frequency selection and suboptimal EAS task placement decisions. When `uclamp_max` is inflated by stale rq values, the scheduler fails to apply the intended frequency cap for tasks with restrictive `uclamp_max` settings. This leads to:

1. **Excessive power consumption**: Tasks that should be frequency-capped (via uclamp_max) end up running at higher frequencies than intended, wasting energy. This is particularly impactful on mobile and embedded platforms (like the Unisoc SoCs where this was discovered) where EAS and uclamp are used extensively for power management. The `find_energy_efficient_cpu()` function uses `uclamp_rq_util_with()` to determine `fits_capacity()` and to compute energy estimates. With inflated uclamp_max, the energy estimate for placing a task on a big CPU may appear acceptable when it should have been capped to a little CPU, leading to unnecessary use of high-performance cores.

2. **Incorrect capacity fitting**: The `fits_capacity()` check in `find_energy_efficient_cpu()` uses the clamped utilization to determine if a CPU can accommodate a task. With an inflated uclamp_max, a task's apparent utilization may exceed a small CPU's capacity, causing the scheduler to skip that CPU even though the task's actual clamped utilization would fit. This can lead to tasks being placed on unnecessarily powerful CPUs or, conversely, CPUs being deemed overutilized when they are not.

3. **Violation of userspace intent**: The entire purpose of `uclamp_max` is to allow userspace (or the thermal framework, or power management daemons) to cap a task's perceived utilization. When this cap is silently ignored on idle CPUs, the contract between userspace and the scheduler is broken. Applications relying on uclamp for thermal management or QoS enforcement will not get the expected behavior.

There is no crash, hang, or data corruption — this is a correctness and performance bug that manifests as degraded power efficiency and suboptimal task placement on systems using EAS with uclamp.

## Fix Summary

The fix modifies `uclamp_rq_util_with()` in `kernel/sched/sched.h` to skip the max aggregation with rq-level uclamp values when the rq is idle and a task `p` is provided. Specifically, the function now:

1. Initializes `min_util` and `max_util` to 0 (instead of leaving them uninitialized).
2. If a task `p` is provided, first computes the task's own effective uclamp values: `min_util = uclamp_eff_value(p, UCLAMP_MIN)` and `max_util = uclamp_eff_value(p, UCLAMP_MAX)`.
3. Checks `rq->uclamp_flags & UCLAMP_FLAG_IDLE` — if the rq is idle, it jumps directly to the output path, bypassing the rq-level aggregation entirely.
4. Only if the rq is *not* idle does it perform the max aggregation: `min_util = max(min_util, rq->uclamp[UCLAMP_MIN].value)` and similarly for max_util.

This is correct because when a task is about to be enqueued on an idle rq, the rq's uclamp values will be reset to the task's values by `uclamp_idle_reset()` called from `uclamp_rq_inc_id()` during enqueue. There is no need to aggregate with the stale values — they represent a task that is no longer present. The task's own effective uclamp values are the only relevant bounds. When no task `p` is provided (i.e., `p == NULL`), the function still reads the rq values as before, since in that case the caller wants the rq-level aggregation only.

The fix is minimal, targeted, and preserves the existing behavior for non-idle rqs and for the `p == NULL` case. It correctly handles the interaction with the `UCLAMP_FLAG_IDLE` mechanism that was introduced to retain max-clamp values during idle for schedutil's benefit.

## Triggering Conditions

The bug requires the following conditions to trigger:

- **CONFIG_UCLAMP_TASK=y**: Uclamp must be enabled in the kernel configuration for the `uclamp_rq_util_with()` function to be active (the `sched_uclamp_used` static key must be enabled, which happens when any task sets a uclamp value).
- **CONFIG_ENERGY_MODEL=y and CONFIG_SMP=y**: The most impactful path is through `find_energy_efficient_cpu()`, which requires EAS to be active. EAS requires an asymmetric CPU topology (big.LITTLE) or at least an energy model.
- **An idle CPU**: The bug triggers when `uclamp_rq_util_with()` is called on a runqueue that has `UCLAMP_FLAG_IDLE` set. This happens after the last task on a CPU has been dequeued. The CPU must be idle at the time the waking task is being evaluated for placement.
- **A task with uclamp_max < previous task's uclamp_max**: The stale rq-level uclamp_max must be higher than the waking task's effective uclamp_max. The most common case is a task with a restricted `uclamp_max` (e.g., 512) waking up on a CPU where the previous task had the default `uclamp_max` of 1024.
- **Task wakeup path**: The task must be in the wakeup path where `find_energy_efficient_cpu()` (or any other caller of `uclamp_rq_util_with()`) evaluates idle CPUs before the task is enqueued.

The bug is highly reproducible on any system with EAS and uclamp enabled. It occurs every single time a uclamp-restricted task wakes up and the scheduler evaluates idle CPUs for placement. The probability of hitting this scenario is very high in real workloads on mobile platforms.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why this bug cannot be reproduced with kSTEP

**The kernel version is too old.** The fix was merged into v5.14-rc1, and the bug existed from v5.3-rc1 through v5.13.x. kSTEP supports Linux v5.15 and newer only. Since v5.14-rc1 predates v5.15, the buggy kernel code does not exist in any kernel version that kSTEP can run. By v5.15, this fix is already applied, and `uclamp_rq_util_with()` already correctly handles the idle rq case.

### 2. What would need to be added to kSTEP

Even if version compatibility were not an issue, reproducing this bug would require:

- **Uclamp task attribute setting**: kSTEP would need a `kstep_task_set_uclamp(p, clamp_id, value)` API to set per-task uclamp_min and uclamp_max values. This is not currently available in kSTEP's task management API, though it would be a straightforward addition (writing to the task's `uclamp_req` fields and calling `uclamp_update_active()`).
- **Energy-aware scheduling (EAS) support**: The primary manifestation path is through `find_energy_efficient_cpu()`, which requires an asymmetric CPU topology with an energy model. kSTEP can configure topology via `kstep_topo_*` and CPU capacities via `kstep_cpu_set_capacity()`, but EAS additionally requires `CONFIG_ENERGY_MODEL` and registered energy model data. kSTEP would need the ability to register energy model entries for performance domains.
- **Observation of uclamp_rq_util_with() return values**: To detect the bug, one would need to observe the return value of `uclamp_rq_util_with()` during task wakeup, specifically in the `find_energy_efficient_cpu()` path. This would require either hooking into the wakeup path or reading `rq->uclamp` values at the right moment. kSTEP provides `KSYM_IMPORT` for accessing internal symbols, but timing the observation to catch the transient state during wakeup evaluation would be challenging.

### 3. Version too old confirmation

The fix targets v5.14-rc1 which is strictly before v5.15. kSTEP requires Linux v5.15 or newer to operate. Therefore, this bug is categorized as unplanned due to kernel version incompatibility.

### 4. Alternative reproduction methods

Outside kSTEP, this bug could be reproduced on a v5.13 or earlier kernel with the following approach:

- Use an ARM big.LITTLE or similar asymmetric platform (or QEMU with asymmetric CPU capacities and an energy model, if supported at that kernel version).
- Enable `CONFIG_UCLAMP_TASK=y` and `CONFIG_ENERGY_MODEL=y`.
- Create two tasks: Task A with default uclamp_max (1024) and Task B with restricted uclamp_max (e.g., 512 via `sched_setattr()`).
- Run Task A on a target CPU, then let it sleep to make the CPU idle.
- Wake Task B and trace `uclamp_rq_util_with()` (e.g., with ftrace or a kprobe) to observe that the returned max_util is 1024 instead of the expected 512 when evaluating the idle CPU.
- Alternatively, use `trace_sched_cpu_capacity_tp` or energy model tracepoints to observe the inflated utilization estimates during EAS placement.
- Compare the placement decision and resulting CPU frequency with the expected behavior under the fix.
