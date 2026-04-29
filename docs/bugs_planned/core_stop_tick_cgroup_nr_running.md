# Core: sched_can_stop_tick() uses wrong nr_running counter with cgroups

**Commit:** `c1f43c342e1f2e32f0620bf2e972e2a9ea0a1e60`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.13-rc3
**Buggy since:** v6.12-rc1 (introduced by commit `11cc374f4643b1be16deab571e034409c6ee7e66` "sched_ext: Simplify scx_can_stop_tick() invocation in sched_can_stop_tick()")

## Bug Description

The function `sched_can_stop_tick()` in `kernel/sched/core.c` determines whether the scheduler tick can be stopped on a given CPU. This function is critical for `CONFIG_NO_HZ_FULL` (full dynticks) operation, where the kernel attempts to stop the periodic timer interrupt on CPUs running only a single task, allowing that task to run uninterrupted for maximum throughput and minimal latency jitter. If this function returns `true` when it should return `false`, the tick is stopped on a CPU that still requires periodic preemption, leading to task starvation.

The bug was introduced when commit `11cc374f4643b` refactored the CFS task count check in `sched_can_stop_tick()` as part of simplifying the `sched_ext` (SCX) interaction. The original code checked `rq->nr_running > 1` (the total number of runnable tasks across ALL scheduling classes on the runqueue). The refactored code changed this to `rq->cfs.nr_running > 1`, intending to check only CFS tasks after the DL and RT checks had already filtered those classes. However, `cfs_rq.nr_running` only counts `sched_entity` objects at the root level of the CFS hierarchy — it does not count tasks nested inside task groups (cgroups). When `CONFIG_FAIR_GROUP_SCHED` is enabled and tasks are placed inside cgroups, the root-level `nr_running` may be 1 (representing the single task group entity) even though multiple actual tasks are runnable within that group.

The fix correctly changes the check to use `rq->cfs.h_nr_running > 1`, which is the hierarchical count that sums all actual CFS tasks across all levels of the task group hierarchy. This ensures that when multiple CFS tasks are runnable (even if they share a single task group entity at the root level), the tick is not stopped and involuntary preemption continues to function.

This patch was submitted as part of a larger 11-patch series ("sched/fair: Fix statistics with delayed dequeue") by Vincent Guittot, but was placed first in the series specifically so it could be easily backported independently. The cover letter notes that `sched_can_stop_tick()` should use the hierarchical count (called `h_nr_queued` after later renames in the series) rather than the root-level entity count.

## Root Cause

The root cause is a semantic mismatch between two counters in `struct cfs_rq`:

1. **`cfs_rq.nr_running`**: Counts the number of `struct sched_entity` objects directly enqueued on this specific `cfs_rq`. At the root CFS runqueue level, when `CONFIG_FAIR_GROUP_SCHED` is enabled, a task group contributes exactly one `sched_entity` regardless of how many tasks are inside the group.

2. **`cfs_rq.h_nr_running`**: The hierarchical count that tracks the total number of actual runnable tasks (not group entities) summed across the entire CFS hierarchy rooted at this `cfs_rq`.

When commit `11cc374f4643b` changed the sched_ext interaction in `sched_can_stop_tick()`, it restructured the logic so that DL and RT tasks are checked first, and if none are present, the remaining tasks must be CFS or SCX. The code was changed from:

```c
/* Old code (before the bug) */
if (!scx_switched_all() && rq->nr_running > 1)
    return false;
```

to:

```c
/* Bug-introducing code (11cc374f4643b) */
if (scx_enabled() && !scx_can_stop_tick(rq))
    return false;

if (rq->cfs.nr_running > 1)
    return false;
```

The problem is that `rq->cfs.nr_running` was used instead of `rq->cfs.h_nr_running`. Consider a scenario where two CFS tasks (Task A and Task B) are placed in a cgroup `/sys/fs/cgroup/mygroup` and pinned to CPU 1:

- The root `cfs_rq` on CPU 1 has `nr_running = 1` (just the one `sched_entity` representing the `mygroup` task group).
- The root `cfs_rq` on CPU 1 has `h_nr_running = 2` (the two actual tasks, counted hierarchically).
- The `mygroup`'s `cfs_rq` on CPU 1 has `nr_running = 2` (both task entities directly enqueued here).

The buggy check `rq->cfs.nr_running > 1` evaluates to `1 > 1` which is `false`. The function does NOT return `false`, and instead falls through to `return true`, incorrectly indicating the tick can be stopped.

The fix changes the single line:

```c
/* Fixed code (c1f43c342e1f) */
if (rq->cfs.h_nr_running > 1)
    return false;
```

Now `rq->cfs.h_nr_running > 1` evaluates to `2 > 1` which is `true`, correctly returning `false` to prevent the tick from being stopped.

It is worth noting that without `CONFIG_FAIR_GROUP_SCHED` (i.e., no task groups), `nr_running` and `h_nr_running` at the root level are always equal since there is only one level in the hierarchy. The bug exclusively manifests when task groups (cgroups with cpu controllers) are in use.

## Consequence

When the bug triggers, the scheduler tick is incorrectly stopped on a CPU that has multiple runnable CFS tasks within a cgroup. Without the periodic tick, the CFS scheduler's involuntary preemption mechanism ceases to function on that CPU. This means:

- **Task starvation**: Whichever CFS task is currently running when the tick stops will monopolize the CPU indefinitely (or until it voluntarily yields, sleeps, or blocks). Other runnable tasks in the same cgroup will starve.
- **Latency spikes**: Tasks waiting for CPU time inside the cgroup will experience unbounded scheduling latency, completely defeating the purpose of CFS fair time sharing.
- **Broken timeslice enforcement**: CFS normally uses the tick to check if a task has exceeded its fair share (via `entity_tick()` → `check_preempt_tick()`). Without ticks, this check never runs, so timeslices are not enforced.

This bug specifically affects systems running with `CONFIG_NO_HZ_FULL` and `CONFIG_FAIR_GROUP_SCHED` simultaneously — a common configuration for latency-sensitive workloads in containerized environments (e.g., Kubernetes pods, Docker containers, systemd service cgroups). The impact is most severe when multiple CPU-bound tasks share a cgroup and are affined to the same CPU, as one task will run to the exclusion of all others.

The bug does not cause a kernel panic, oops, or data corruption. However, it silently violates the fundamental fairness guarantee of CFS, which can be extremely difficult to diagnose in production. An affected workload might appear to have one "stuck" thread while others make no progress, with no obvious kernel error messages.

## Fix Summary

The fix is a single-line change in `sched_can_stop_tick()` in `kernel/sched/core.c`, replacing:

```c
if (rq->cfs.nr_running > 1)
```

with:

```c
if (rq->cfs.h_nr_running > 1)
```

This changes the CFS task count check from the root-level entity count (`nr_running`) to the hierarchical task count (`h_nr_running`). The `h_nr_running` field is maintained by the enqueue/dequeue paths in `fair.c` — it is incremented in `enqueue_task_fair()` and decremented in `dequeue_entities()` as tasks traverse the hierarchy. It accurately reflects the total number of CFS tasks that are runnable on this CPU, regardless of how many levels of task group nesting exist.

The fix is correct because the purpose of the check is to determine if there are multiple CFS tasks competing for the CPU, which requires involuntary preemption (and thus the tick). The hierarchical count `h_nr_running` answers exactly this question: "how many actual runnable CFS tasks are on this CPU?" If the answer is more than 1, the tick must continue to enforce fair scheduling.

The fix is also minimal and safe — it changes only the field being read, not the logic structure. It is a strict improvement: in the non-cgroup case (no `CONFIG_FAIR_GROUP_SCHED` or all tasks at root level), `nr_running == h_nr_running` so behavior is unchanged. Only the cgroup case is corrected.

## Triggering Conditions

The following conditions must ALL be met simultaneously to trigger the bug:

1. **CONFIG_NO_HZ_FULL=y**: The kernel must be built with full dynticks support. Without this, `sched_can_stop_tick()` does not exist and the tick is always periodic.

2. **CONFIG_FAIR_GROUP_SCHED=y**: Task groups (cgroup CPU scheduling) must be enabled. Without this, `nr_running` and `h_nr_running` are always equal at the root level.

3. **CONFIG_SMP=y**: `sched_can_stop_tick()` is gated behind `CONFIG_SMP`.

4. **A cgroup with 2 or more CFS tasks on the same CPU**: There must be a task group (cgroup with `cpu` controller) containing at least 2 runnable CFS tasks pinned to (or both scheduled on) the same CPU. The tasks must be SCHED_NORMAL, SCHED_BATCH, or SCHED_IDLE policy.

5. **No DL or RT tasks on that CPU**: The function checks DL and RT tasks first. If any DL task is present, the function returns `false` (tick needed) before reaching the buggy CFS check. If any RR tasks are present (more than 1), the same applies. To reach the buggy CFS check, the CPU must have zero DL tasks, zero RT tasks, and only CFS (and/or SCX) tasks.

6. **No SCX blocking the tick**: If `sched_ext` is enabled and `scx_can_stop_tick()` returns `false`, the function already returns `false` before reaching the buggy CFS check. So either SCX must not be enabled, or `scx_can_stop_tick()` must return `true`.

7. **The CPU must be a `nohz_full` CPU**: At runtime, the CPU must be designated as a full dynticks CPU (via `nohz_full=` boot parameter or equivalent). Otherwise, the NO_HZ_FULL logic is not active and the tick is not stopped regardless of `sched_can_stop_tick()`'s return value.

The bug is fully deterministic given these conditions — there is no race condition or timing dependency. Whenever 2+ CFS tasks are runnable inside a cgroup on a nohz_full CPU with no DL/RT tasks, the tick will be incorrectly stopped.

## Reproduce Strategy (kSTEP)

This bug can be reproduced in kSTEP by creating the necessary cgroup hierarchy with multiple CFS tasks and directly calling `sched_can_stop_tick()` to observe its incorrect return value on the buggy kernel.

### Prerequisite: Enable CONFIG_NO_HZ_FULL

The kSTEP kernel config (`linux/config.kstep`) currently has `# CONFIG_NO_HZ_FULL is not set`. To reproduce this bug, `CONFIG_NO_HZ_FULL=y` must be enabled. This can be done by either:
- Modifying `linux/config.kstep` to set `CONFIG_NO_HZ_FULL=y` (and also `CONFIG_NO_HZ=y` since NO_HZ_FULL depends on it)
- Or passing `KSTEP_EXTRA_CONFIG` pointing to a fragment file with these options

This is a minor kernel config change. Note: `CONFIG_CONTEXT_TRACKING_USER=y` may also be needed as a dependency of `CONFIG_NO_HZ_FULL`. The kernel's `Kconfig` will resolve dependencies automatically when using `merge_config.sh`.

### QEMU Configuration

Configure QEMU with at least 2 CPUs. CPU 0 is reserved for the driver; tasks will be placed on CPU 1. No special memory or NUMA configuration is needed.

### Driver Setup (setup callback)

1. **Import `sched_can_stop_tick`**: Use `KSYM_IMPORT(sched_can_stop_tick)` or `KSYM_IMPORT_TYPED(bool (*)(struct rq *), sched_can_stop_tick)` to obtain a function pointer to the kernel's `sched_can_stop_tick()` function via kallsyms.

2. **Create a cgroup**: Call `kstep_cgroup_create("testgrp")` to create a cgroup.

3. **Create two CFS tasks**: Call `kstep_task_create()` twice to create two tasks (Task A and Task B). Both will be SCHED_NORMAL (CFS) by default.

4. **Pin both tasks to CPU 1**: Call `kstep_task_pin(taskA, 1, 2)` and `kstep_task_pin(taskB, 1, 2)` to confine both tasks to CPU 1.

5. **Add both tasks to the cgroup**: Call `kstep_cgroup_add_task("testgrp", taskA->pid)` and `kstep_cgroup_add_task("testgrp", taskB->pid)` to place both tasks in the test cgroup.

### Driver Run Sequence (run callback)

1. **Wake both tasks**: Call `kstep_task_wakeup(taskA)` and `kstep_task_wakeup(taskB)` to make both tasks runnable on CPU 1.

2. **Advance a few ticks**: Call `kstep_tick_repeat(5)` to allow the scheduler to stabilize and both tasks to be fully enqueued.

3. **Read internal state for verification**:
   - Access `cpu_rq(1)->cfs.nr_running` — should be 1 (the task group entity at root level).
   - Access `cpu_rq(1)->cfs.h_nr_running` — should be 2 (two actual tasks hierarchically).
   - Log both values with `TRACE_INFO()` for debugging.

4. **Call sched_can_stop_tick**: Call `KSYM_sched_can_stop_tick(cpu_rq(1))` and capture the return value.

5. **Check result**:
   - **On the buggy kernel**: `sched_can_stop_tick()` returns `true` because it checks `rq->cfs.nr_running > 1` which is `1 > 1 = false`, so it falls through to `return true`. This is WRONG — there are 2 tasks that need preemption. Call `kstep_fail("sched_can_stop_tick returned true with %d hierarchical CFS tasks in cgroup", h_nr)`.
   - **On the fixed kernel**: `sched_can_stop_tick()` returns `false` because it checks `rq->cfs.h_nr_running > 1` which is `2 > 1 = true`, correctly indicating the tick should not stop. Call `kstep_pass("sched_can_stop_tick correctly returned false with %d hierarchical CFS tasks", h_nr)`.

6. **Pass/fail criteria**: The driver should `kstep_fail` if `sched_can_stop_tick()` returns `true` when `cpu_rq(1)->cfs.h_nr_running > 1`. It should `kstep_pass` if `sched_can_stop_tick()` returns `false` under these conditions.

### Alternative Approach (Without CONFIG_NO_HZ_FULL)

If enabling `CONFIG_NO_HZ_FULL` proves impractical (e.g., due to QEMU compatibility issues with full dynticks), an alternative approach can demonstrate the bug's data condition:

1. Set up the same cgroup hierarchy with 2 tasks on CPU 1.
2. Read `cpu_rq(1)->cfs.nr_running` and `cpu_rq(1)->cfs.h_nr_running`.
3. Assert that `nr_running == 1` and `h_nr_running == 2`.
4. On the **buggy kernel**: The field `nr_running` is used in the check, so replicate the buggy logic: `bool would_stop = !(rq->cfs.nr_running > 1)`. This evaluates to `true` (would incorrectly stop tick). Call `kstep_fail()`.
5. On the **fixed kernel**: The field `h_nr_running` is used: `bool would_stop = !(rq->cfs.h_nr_running > 1)`. This evaluates to `false` (would correctly keep tick). Call `kstep_pass()`.

However, note that this alternative does NOT call the actual kernel function and instead replicates the logic in the driver. The data values (`nr_running=1`, `h_nr_running=2`) are the same on both kernels — only the code path differs. So this approach would need to read the actual kernel code at the call site to determine which field is used, which is not straightforward. The KSYM_IMPORT approach (with CONFIG_NO_HZ_FULL) is strongly preferred.

### Expected Behavior Summary

| Kernel | `cfs.nr_running` | `cfs.h_nr_running` | `sched_can_stop_tick()` | Correct? |
|--------|------------------|--------------------|------------------------|----------|
| Buggy  | 1                | 2                  | `true`                 | NO — tick stops, tasks starve |
| Fixed  | 1                | 2                  | `false`                | YES — tick continues, fair scheduling |

### Guard Macro

The driver should be guarded with `#if LINUX_VERSION_CODE` appropriate for the v6.12-rc1 to v6.13-rc3 range where this bug exists. The `sched_can_stop_tick()` function requires `CONFIG_NO_HZ_FULL`, so the driver should also check `#ifdef CONFIG_NO_HZ_FULL` or gracefully handle the case where the KSYM_IMPORT fails to resolve the symbol.
