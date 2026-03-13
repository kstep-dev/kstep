# Energy: Overutilized update for new tasks is a no-op due to flags overwrite

**Commit:** `8e1ac4299a6e8726de42310d9c1379f188140c71`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.10-rc5
**Buggy since:** v5.0-rc1 (introduced by `2802bf3cd936` "sched/fair: Add over-utilization/tipping point indicator")

## Bug Description

The Linux kernel's Energy Aware Scheduling (EAS) framework uses an "overutilized" flag on the root domain (`rq->rd->overutilized`) to decide whether to use energy-efficient task placement or traditional load-based balancing. When the system is not overutilized, EAS places tasks on the most energy-efficient CPU. When overutilized (i.e., any CPU's utilization exceeds its capacity), EAS is bypassed and traditional load balancing takes over to preserve `smp_nice` guarantees.

When a new task is created via `fork()`, the scheduler assigns it an initial `util_avg` equal to half the spare capacity of the CPU it is placed on. This initial estimate can be large enough to push a CPU over the overutilization threshold, which would incorrectly trigger the load balancer to override EAS task placement decisions. To mitigate this, `enqueue_task_fair()` was designed to skip the `update_overutilized_status()` call for newly forked tasks (whose PELT signals have not yet converged to reflect actual workload).

However, a logic error in `enqueue_task_fair()` caused this skip condition to never actually fire. The `flags` variable, which is checked at the end of the function to determine if the task is new, is overwritten to `ENQUEUE_WAKEUP` in the middle of the function's `for_each_sched_entity` loop. As a result, `flags` always contains `ENQUEUE_WAKEUP` by the time the overutilized check is reached, regardless of whether the enqueue was due to a wakeup or a fork. The condition `if (flags & ENQUEUE_WAKEUP)` was therefore always true, making it a no-op guard that never skipped the overutilized update.

## Root Cause

The root cause lies in the `enqueue_task_fair()` function in `kernel/sched/fair.c`. The function walks up the scheduling entity hierarchy using a `for_each_sched_entity(se)` loop to enqueue the task at each level of the cgroup hierarchy. Inside this first loop, after processing the first entity, the line:

```c
flags = ENQUEUE_WAKEUP;
```

unconditionally overwrites the `flags` variable. This is intentional for the entity-level `enqueue_entity()` calls — parent entities should be treated as "woken up" regardless of the original reason for enqueueing the child task. However, this overwrite has a destructive side effect on the later overutilized check.

After both `for_each_sched_entity` loops complete, the function reaches the overutilized guard:

```c
if (flags & ENQUEUE_WAKEUP)
    update_overutilized_status(rq);
```

The intent is: if the task is being enqueued because of a wakeup (`ENQUEUE_WAKEUP` set in the original flags), call `update_overutilized_status()`. If the task is new (fork enqueue, where `ENQUEUE_WAKEUP` is NOT set), skip the check. But by this point, `flags` has already been set to `ENQUEUE_WAKEUP` by the loop body, so the condition `flags & ENQUEUE_WAKEUP` is always true for any task that has at least one scheduling entity level (which is all tasks). The new-task exclusion is dead code.

The specific code flow for a newly forked task:
1. `enqueue_task_fair()` is called with `flags` that does NOT include `ENQUEUE_WAKEUP` (a fork enqueue).
2. The first `for_each_sched_entity` loop processes the bottom-level `sched_entity`. After the first entity is enqueued, `flags = ENQUEUE_WAKEUP;` executes.
3. All subsequent entities in the hierarchy and the final overutilized check see `flags` containing `ENQUEUE_WAKEUP`.
4. The guard `if (flags & ENQUEUE_WAKEUP)` evaluates to true, and `update_overutilized_status()` is called even though this is a new task.

## Consequence

The observable impact of this bug is that newly forked tasks incorrectly trigger the overutilized check. On systems using Energy Aware Scheduling (typically ARM big.LITTLE or DynamIQ heterogeneous platforms), this causes the system to be prematurely marked as overutilized whenever a new task is created.

When the overutilized flag is set, EAS is disabled and the load balancer falls back to traditional load-based balancing. This means that on EAS-enabled systems, every `fork()` call has the potential to disable EAS temporarily, defeating the purpose of energy-aware task placement. The result is suboptimal task placement and increased energy consumption. Tasks that EAS would place on energy-efficient little cores may instead be spread across all cores by the load balancer.

As noted by Valentin Schneider in the review thread, the practical impact is somewhat mitigated because the next scheduler tick (within 4ms on arm64) unconditionally re-evaluates the overutilized status. However, during that window, task placement decisions made by EAS may be overridden. On workloads with frequent fork operations (e.g., shell scripts, server workloads spawning child processes), this could cause a constant stream of overutilized false positives, effectively keeping EAS permanently disabled.

## Fix Summary

The fix is minimal and elegant: it saves the new-task status into a local variable at the very beginning of `enqueue_task_fair()`, before the `flags` variable can be overwritten by the loop.

A new local variable `task_new` is introduced at the top of the function:

```c
int task_new = !(flags & ENQUEUE_WAKEUP);
```

This captures whether the original `flags` parameter lacks `ENQUEUE_WAKEUP` (indicating a fork enqueue) before the first `for_each_sched_entity` loop has a chance to overwrite `flags`. Then, the overutilized guard condition is changed from `if (flags & ENQUEUE_WAKEUP)` to `if (!task_new)`, which correctly checks the saved state rather than the mutated `flags` variable.

With this fix, when a new task is enqueued via fork, `task_new` is 1 (true), `!task_new` is 0 (false), and `update_overutilized_status()` is correctly skipped. When an existing task is enqueued via wakeup, `task_new` is 0 (false), `!task_new` is 1 (true), and `update_overutilized_status()` is correctly called. The fix restores the original design intent documented in the comment block above the check.

## Triggering Conditions

The bug is triggered whenever a new CFS task is created via `fork()` on a system with `CONFIG_SMP` enabled. The specific conditions are:

- **CONFIG_SMP=y**: The `update_overutilized_status()` function is compiled out on UP systems (the `#else` stub is empty).
- **A CFS (SCHED_NORMAL/SCHED_BATCH/SCHED_IDLE) task is forked**: The initial `util_avg` assignment and the `enqueue_task_fair()` code path are specific to CFS tasks.
- **The task's initial util_avg pushes the CPU over its capacity**: New tasks get `util_avg = (cpu_capacity - cfs_rq->avg.util_avg) / 2`. If the CPU is already moderately loaded, even this halved spare capacity can exceed the overutilization threshold.
- **EAS is active**: The bug's impact is only meaningful on systems where EAS is enabled (requires an energy model registered via `dev_pm_opp_of_register_em()` or equivalent, plus the `schedutil` cpufreq governor). Without EAS, the overutilized flag has no effect on scheduling decisions.

The bug requires no special timing, race conditions, or unusual workload patterns. It occurs deterministically on every fork operation. The overutilized flag will be incorrectly set if the CPU's utilization plus the new task's initial `util_avg` exceeds the CPU's capacity, which is the exact scenario the skip was designed to prevent.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The bug was introduced in v5.0-rc1 (commit `2802bf3cd936`) and fixed in v5.10-rc5 (commit `8e1ac4299a6e8726de42310d9c1379f188140c71`). kSTEP supports Linux v5.15 and newer only. By v5.15, this fix was already merged into the mainline kernel. There is no kernel version ≥ v5.15 that contains this bug, making it impossible to reproduce within kSTEP's supported version range.

2. **WHAT would need to be added to kSTEP to support this?**
   No kSTEP framework changes would help. The fundamental issue is that the bug does not exist in any kernel version that kSTEP can run. If kSTEP's minimum supported version were lowered to v5.9 or earlier, the bug could be reproduced by:
   - Creating a kSTEP driver that forks a CFS task (using `kstep_task_create()` and `kstep_task_fork()`)
   - Setting up an asymmetric CPU topology with `kstep_topo_*()` and `kstep_cpu_set_capacity()`
   - Loading the CPU to near-capacity before the fork
   - Checking `rq->rd->overutilized` via `KSYM_IMPORT` after the fork enqueue
   - Verifying that overutilized is incorrectly set on the buggy kernel and correctly not set on the fixed kernel

3. **Version constraint:** The fix targets v5.10-rc5, which is before kSTEP's minimum supported version of v5.15. The bug cannot exist on any kernel that kSTEP can boot.

4. **Alternative reproduction methods:**
   - Boot a v5.9 or v5.10-rc4 kernel on real ARM hardware (or QEMU with an energy model) with EAS enabled.
   - Run a workload that forks many short-lived tasks (e.g., a shell script running many commands in a loop).
   - Trace `update_overutilized_status()` calls using ftrace to observe that it fires on every fork, including new tasks.
   - Compare with a v5.10-rc5+ kernel where the overutilized update is correctly skipped for new tasks.
   - Alternatively, add a `tracepoint` or `printk` inside `enqueue_task_fair()` to log the `flags` value and `task_new` status at the overutilized check point.
