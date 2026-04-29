# Energy: Misfit status on pinned task inflates balance_interval

**Commit:** `0ae78eec8aa64e645866e75005162603a77a0f49`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.12-rc1
**Buggy since:** v4.19-rc3 (introduced by `3b1baa6496e6` "sched/fair: Add 'group_misfit_task' load-balance type")

## Bug Description

On asymmetric CPU capacity systems (e.g., ARM big.LITTLE), the scheduler's "misfit task" mechanism detects when a task's utilization exceeds the capacity of its current CPU, flagging it for migration to a higher-capacity CPU. The function `update_misfit_status()` in `kernel/sched/fair.c` sets `rq->misfit_task_load` to a non-zero value when the currently running task does not fit on its CPU. This misfit flag triggers load balancing attempts to migrate the task to a bigger CPU.

However, if the task is pinned to a single CPU (via `sched_setaffinity()` or `cpuset` with a single CPU), the migration attempt is guaranteed to fail because the task's `cpus_ptr` does not allow it to move to any other CPU. Despite this, `update_misfit_status()` still sets `rq->misfit_task_load`, causing the load balancer to repeatedly attempt (and fail) to migrate the task.

Each failed load balance attempt causes the scheduler to double `sd->balance_interval` at the `out_one_pinned` exit path in `load_balance()`. After many consecutive failures, `balance_interval` can grow exponentially to a very large value (up to `MAX_PINNED_INTERVAL` or `sd->max_interval`). Once the pinned misfit task finishes or unpins, any genuinely misfit or imbalanced tasks on the system experience significant delays before the next load balance attempt, because the inflated `balance_interval` persists.

The bug was discovered during uclamp testing on an ARM system, where an rt-app calibration loop was pinned to CPU 0 (a little core). The calibration task's utilization exceeded the little core's capacity, triggering the misfit status. After the calibration completed and a new task with a high `util_clamp` value was spawned, that task failed to migrate for over 40ms because `balance_interval` had been unnecessarily inflated by the repeated failed migration attempts of the pinned calibration task.

## Root Cause

The root cause is in the `update_misfit_status()` function in `kernel/sched/fair.c`:

```c
static inline void update_misfit_status(struct task_struct *p, struct rq *rq)
{
    if (!static_branch_unlikely(&sched_asym_cpucapacity))
        return;

    if (!p) {
        rq->misfit_task_load = 0;
        return;
    }

    if (task_fits_capacity(p, capacity_of(cpu_of(rq)))) {
        rq->misfit_task_load = 0;
        return;
    }

    rq->misfit_task_load = max_t(unsigned long, task_h_load(p), 1);
}
```

This function is called from three sites:
1. `pick_next_task_fair()` (line ~7151) — called when picking the next CFS task to run, passing the selected task `p`.
2. `task_tick_fair()` (line ~10774) — called on every scheduler tick for the currently running task `curr`.
3. `newidle_balance()` (line ~10589) — called with `NULL` to clear misfit status when the CPU goes idle.

The function checks whether `sched_asym_cpucapacity` is enabled (i.e., the system has asymmetric CPU capacities) and whether the task fits on its current CPU via `task_fits_capacity()`. If the task does not fit, it sets `rq->misfit_task_load` to the task's hierarchical load. The critical omission is that it never checks whether the task is actually migratable. When `p->nr_cpus_allowed == 1`, the task is pinned to exactly one CPU and cannot be migrated anywhere, yet the function still marks the runqueue as having a misfit task.

The consequence cascades through the load balancing path. When `check_misfit_status()` returns true (because `rq->misfit_task_load != 0`), the load balancer attempts to pull/push the task. In `load_balance()`, when migration fails because the task is pinned (the `LBF_ALL_PINNED` flag is set), execution reaches the `out_one_pinned` label:

```c
out_one_pinned:
    ld_moved = 0;
    if (env.idle == CPU_NEWLY_IDLE)
        goto out;

    if ((env.flags & LBF_ALL_PINNED &&
         sd->balance_interval < MAX_PINNED_INTERVAL) ||
        sd->balance_interval < sd->max_interval)
        sd->balance_interval *= 2;
```

This doubles `sd->balance_interval` on every failed attempt. Since the pinned misfit task continually triggers load balancing through `check_misfit_status()`, and every attempt fails, `balance_interval` grows exponentially. On a system with many ticks between genuine balance opportunities, this can rapidly escalate to values that cause multi-second delays in load balancing.

## Consequence

The observable impact is a significant delay in load balancing for other tasks on the system. After the pinned misfit task has caused `balance_interval` to inflate, any subsequent task that genuinely needs migration (whether for misfit reasons, load balancing, or other imbalance) must wait until the inflated balance interval expires before the load balancer runs again.

In the reported scenario, the delay was over 40ms — a task with a high `util_clamp` value could not migrate to a bigger CPU for more than 40ms of wall-clock time. On latency-sensitive workloads (interactive applications, real-time audio/video processing on mobile devices), this kind of delay is severely detrimental. On big.LITTLE ARM SoCs commonly used in mobile phones, this directly impacts user experience: a foreground task might be stuck on a little core when it should be on a big core, causing visible jank or audio glitches.

The issue is particularly insidious because it is a "delayed" effect — the damage (inflated `balance_interval`) persists after the pinned task is gone. There is no crash or kernel warning; the system simply becomes sluggish at responding to load imbalances. The only recovery is waiting for `balance_interval` to naturally reset when a successful balance eventually occurs (which resets `sd->balance_interval = sd->min_interval`), but this could take a very long time if the interval has grown large enough.

In extreme cases on systems where pinned misfit tasks are common (e.g., containerized workloads with `cpuset` constraints on asymmetric hardware), the `balance_interval` could remain persistently inflated, causing chronic load balancing delays across the entire scheduling domain.

## Fix Summary

The fix adds a single condition to `update_misfit_status()` to clear the misfit status when the task is pinned to a single CPU:

```c
-   if (!p) {
+   if (!p || p->nr_cpus_allowed == 1) {
        rq->misfit_task_load = 0;
        return;
    }
```

When `p->nr_cpus_allowed == 1`, the task can only run on its current CPU, so marking it as misfit is pointless — no migration can resolve the misfit condition. By treating this case the same as `!p` (clearing `rq->misfit_task_load`), the load balancer is never triggered for this unmovable task, and `balance_interval` is not unnecessarily inflated.

This fix is correct and minimal. It addresses the root cause directly: the misfit flag should only be set when migration is actually possible. The commit message also notes a potential future improvement — checking not just `nr_cpus_allowed == 1` but whether the task's allowed CPUs include any CPU with sufficient capacity. A task affined to multiple little cores would still trigger the same problem, though with `nr_cpus_allowed > 1` the migration might move it between little cores without solving the misfit condition. That more comprehensive check was left for future work.

The fix was reviewed and acked by Valentin Schneider and Quentin Perret (ARM and Google engineers working on EAS/misfit), and signed off by Peter Zijlstra. Vincent Guittot raised a side question about the case where two misfit tasks are on the same CPU (one pinned, one not), but the function only operates on the current running task, so the non-pinned task would get its misfit status set when it becomes current.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger this bug:

- **Asymmetric CPU capacity system:** The system must have `sched_asym_cpucapacity` enabled, meaning CPUs with different compute capacities (e.g., ARM big.LITTLE, DynamIQ, or Intel hybrid with different P-core/E-core capacities). Without this static key being set, `update_misfit_status()` returns immediately.

- **A task pinned to a single CPU:** A CFS task must have `nr_cpus_allowed == 1`, typically set via `sched_setaffinity()` to a single CPU, or by being in a cpuset with exactly one CPU. The pinned CPU must be a "small" CPU whose capacity is less than the task's utilization.

- **Task utilization exceeding CPU capacity:** The pinned task's utilization (as computed by `uclamp_task_util(p)`) must exceed the capacity of the CPU it is pinned to. This is checked via `task_fits_capacity()` which calls `fits_capacity(uclamp_task_util(p), capacity_of(cpu_of(rq)))`. The `fits_capacity` macro includes a 20% margin: the task "fits" if its utilization is at most 80% of the CPU's capacity.

- **Sustained running on the small CPU:** The pinned task must run for long enough (multiple scheduler ticks) for `update_misfit_status()` to be called repeatedly via `task_tick_fair()`, and for the load balancer to make multiple failed migration attempts, each doubling `balance_interval`.

- **Another task needing migration afterward:** The consequence is only observable when, after the pinned misfit task has inflated `balance_interval`, another task genuinely needs load balancing. Without such a task, the inflated interval has no visible effect.

The reproduction probability is deterministic given the conditions above: on an asymmetric system, pinning a high-utilization task to a small CPU will always trigger the misfit flag and cause `balance_interval` inflation. The effect is proportional to how long the pinned task runs — longer durations allow more doublings of `balance_interval`.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP because the fix was merged in **v5.12-rc1**, and kSTEP only supports Linux **v5.15 and newer**. The bug does not exist in any kernel version that kSTEP can run.

**1. Why can this bug not be reproduced with kSTEP?**

The fix commit `0ae78eec8aa64e645866e75005162603a77a0f49` was merged between v5.11-rc2 and v5.12-rc1 (January 2021). It is present in all kernels from v5.12 onward. Since kSTEP requires Linux v5.15 as its minimum supported kernel version, there is no kernel version available to kSTEP that contains the buggy code. Running the driver on any kSTEP-compatible kernel would always show the "fixed" behavior — `update_misfit_status()` already clears `misfit_task_load` for pinned tasks.

**2. What would need to change in kSTEP to support this?**

The only change needed would be support for older kernel versions (pre-v5.12). kSTEP's minimum version would need to be lowered to at least v5.11 or earlier. This is a fundamental infrastructure constraint, not a missing API.

If the kernel version constraint were not an issue, the bug would actually be quite reproducible with kSTEP's existing capabilities:
- Use `kstep_cpu_set_capacity()` to create an asymmetric topology (e.g., CPU 1 at capacity 512, CPU 2 at capacity 1024).
- Use `kstep_task_create()` and `kstep_task_pin(p, 1, 2)` to pin a task to CPU 1 (the small core).
- Drive the task's utilization high enough via sustained running (`kstep_tick_repeat()`).
- Use `KSYM_IMPORT` to read `cpu_rq(1)->misfit_task_load` and the sched_domain's `balance_interval`.
- Observe that `misfit_task_load` is non-zero and `balance_interval` escalates on buggy kernels, while both stay nominal on fixed kernels.

**3. Alternative reproduction methods outside kSTEP:**

On a real ARM big.LITTLE system (or QEMU with asymmetric CPU capacity emulation running a pre-v5.12 kernel):
1. Boot with an asymmetric CPU configuration.
2. Pin an rt-app or stress-ng task to a little core using `taskset -c 0 stress-ng --cpu 1`.
3. Wait several seconds for `balance_interval` to inflate (monitor via `/proc/schedstat` or ftrace `sched:sched_load_balance` tracepoint).
4. Spawn a new high-utilization task and measure how long it takes to migrate to a big core.
5. On buggy kernels, the migration delay will be significantly longer (10s of ms) compared to fixed kernels.

**4. Version constraint summary:**

This is a Category A (KERNEL VERSION TOO OLD) unplanned bug. The fix targets v5.12-rc1, which predates kSTEP's minimum supported version of v5.15.
