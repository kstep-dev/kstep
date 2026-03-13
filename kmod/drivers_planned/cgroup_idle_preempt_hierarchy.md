# Cgroup: SCHED_IDLE Wakeup Preemption Ignores Cgroup Hierarchy

**Commit:** `faa42d29419def58d3c3e5b14ad4037f0af3b496`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.12-rc1
**Buggy since:** v5.15-rc1 (commit `304000390f88` "sched: Cgroup SCHED_IDLE support")

## Bug Description

The wakeup preemption logic in `check_preempt_wakeup_fair()` makes incorrect scheduling decisions when SCHED_IDLE per-task policy interacts with cgroup-level SCHED_IDLE (`cpu.idle=1`). The root of the problem is that the buggy code checks the task-level scheduling policy (`task_has_idle_policy()`) **before** walking the cgroup hierarchy via `find_matching_se()`, causing it to make preemption decisions based on the individual task's policy rather than the matched scheduling entity's idle status within the cgroup tree.

Consider the following cgroup hierarchy:

```
                       root
                        |
             ------------------------
             |                      |
       normal_cgroup            idle_cgroup (cpu.idle=1)
             |                      |
   SCHED_IDLE task_A           SCHED_NORMAL task_B
```

According to the cgroup hierarchy, task_A (in a normal-weight cgroup) should dominate task_B (in an idle cgroup). When task_A wakes up while task_B is running on the same CPU, task_A should preempt task_B because the normal cgroup is strictly higher priority than the idle cgroup. However, the buggy code examines `task_has_idle_policy(p)` at the task level first, sees that task_A is SCHED_IDLE, and prevents it from preempting — completely ignoring the cgroup hierarchy.

Conversely, when task_B (SCHED_NORMAL in idle_cgroup) wakes up while task_A (SCHED_IDLE in normal_cgroup) is running, the buggy code checks `task_has_idle_policy(curr)`, finds it true, and immediately grants preemption to task_B. This is wrong because task_B's cgroup is idle and should not preempt a task in a normal cgroup, regardless of the task-level policy.

This bug breaks the fundamental design principle established by commit `304000390f88`: that cgroup-level SCHED_IDLE should operate in the cgroup hierarchy, with the matched scheduling entities determining relative priority, not the individual task policies. The SCHED_IDLE task policy was intended to be relative within a task's own cgroup (similar to nice values), not an absolute global priority modifier that overrides cgroup hierarchy.

## Root Cause

The root cause is in `check_preempt_wakeup_fair()` in `kernel/sched/fair.c`. The buggy code has two early-exit checks that examine task-level scheduling policy before the `find_matching_se()` call:

```c
/* Idle tasks are by definition preempted by non-idle tasks. */
if (unlikely(task_has_idle_policy(curr)) &&
    likely(!task_has_idle_policy(p)))
    goto preempt;

/*
 * Batch and idle tasks do not preempt non-idle tasks (their preemption
 * is driven by the tick):
 */
if (unlikely(p->policy != SCHED_NORMAL) || !sched_feat(WAKEUP_PREEMPTION))
    return;
```

These two checks operate directly on `curr` (the currently running task) and `p` (the waking task) **before** the code calls `find_matching_se(&se, &pse)`. The `find_matching_se()` function walks the scheduling entity hierarchy to find the pair of entities at the same level that should be compared — this is essential for respecting cgroup hierarchy. Without doing this walk first, the checks compare task-level properties that may be irrelevant at the cgroup level.

The first check (`task_has_idle_policy(curr) && !task_has_idle_policy(p)`) causes any SCHED_IDLE current task to be unconditionally preempted by any non-SCHED_IDLE waking task, regardless of cgroup hierarchy. In the scenario above, when task_B (SCHED_NORMAL in idle_cgroup) wakes while task_A (SCHED_IDLE in normal_cgroup) runs, this check fires and jumps to `preempt`, giving task_B the CPU even though its cgroup is idle.

The second check (`p->policy != SCHED_NORMAL`) causes any SCHED_IDLE or SCHED_BATCH waking task to be prevented from preempting, regardless of cgroup hierarchy. In the scenario above, when task_A (SCHED_IDLE in normal_cgroup) wakes while task_B (SCHED_NORMAL in idle_cgroup) runs, this check returns early because `p->policy == SCHED_IDLE != SCHED_NORMAL`, preventing task_A from preempting even though its cgroup is normal-priority.

Additionally, the `se_is_idle()` function in the `!CONFIG_FAIR_GROUP_SCHED` case unconditionally returned 0, meaning it never identified task-level SCHED_IDLE status. This secondary bug meant that even after the hierarchy walk, the idle-entity comparison (`cse_is_idle` / `pse_is_idle`) would never detect per-task SCHED_IDLE on systems without cgroup support.

The combination of these issues means that cgroup SCHED_IDLE semantics (introduced in `304000390f88`) never fully worked correctly when combined with per-task SCHED_IDLE policy in the wakeup preemption path.

## Consequence

The observable impact is **incorrect preemption decisions** that violate the cgroup scheduling hierarchy. Tasks in idle cgroups can unexpectedly preempt tasks in normal cgroups if the task in the normal cgroup has SCHED_IDLE policy, and vice versa. This leads to **priority inversion** at the cgroup level: workloads placed in idle cgroups (intended to be low priority) can steal CPU time from workloads in normal cgroups, defeating the purpose of the cgroup idle configuration.

In production environments, this affects systems that use cgroup `cpu.idle=1` to isolate low-priority background work (e.g., batch processing, garbage collection) from foreground services. If a foreground service uses SCHED_IDLE for some of its internal background threads (which is a legitimate and common practice — using task-level SCHED_IDLE within a normal-priority cgroup to deprioritize auxiliary threads relative to the main service threads), those threads would be unfairly deprioritized against SCHED_NORMAL tasks in idle cgroups. The SCHED_IDLE task's cgroup priority is ignored, and the idle cgroup's task gets to preempt it.

This bug does not cause crashes, kernel panics, or data corruption. It is a scheduling quality bug that causes incorrect priority enforcement. The worst case is sustained priority inversion where tasks in idle cgroups consistently preempt tasks in normal cgroups, leading to starvation of the normal-cgroup tasks during contention. The secondary `se_is_idle()` bug (returning 0 in the `!CONFIG_FAIR_GROUP_SCHED` case) also means that on non-cgroup kernels, the idle/non-idle entity comparison in the wakeup path was completely non-functional, though this had limited practical impact since without cgroups, the task-level checks (which were the only path) happened to produce mostly correct results.

## Fix Summary

The fix restructures `check_preempt_wakeup_fair()` to perform all preemption decisions **after** the cgroup hierarchy walk, using the matched scheduling entities rather than raw task-level policies. Specifically:

1. **Removes the two early task-level checks**: The `task_has_idle_policy(curr)` preemption check and the `p->policy != SCHED_NORMAL` early return are both deleted. These were the source of the incorrect behavior because they operated before `find_matching_se()`.

2. **Moves WAKEUP_PREEMPTION check up**: The `!sched_feat(WAKEUP_PREEMPTION)` check is separated from the policy check and placed before `find_matching_se()`, since it's a global feature toggle that doesn't depend on entity comparison.

3. **Unifies idle preemption through `se_is_idle()`**: After `find_matching_se()`, the code uses `cse_is_idle = se_is_idle(se)` and `pse_is_idle = se_is_idle(pse)` to determine idle status at the correct hierarchy level. For group entities, `se_is_idle()` checks `cfs_rq_is_idle(group_cfs_rq(se))` (the cgroup's idle flag). For task entities, it checks `task_has_idle_policy(task_of(se))`. This means the idle comparison respects the cgroup hierarchy naturally.

4. **Moves SCHED_BATCH check after idle comparison**: The check for `p->policy != SCHED_NORMAL` (which catches SCHED_BATCH) is placed after the idle-entity comparison, ensuring SCHED_BATCH tasks still don't preempt non-idle entities but the check happens at the right point in the logic flow.

5. **Fixes `se_is_idle()` for `!CONFIG_FAIR_GROUP_SCHED`**: Changes the stub from `return 0` to `return task_has_idle_policy(task_of(se))`, so that per-task SCHED_IDLE is properly detected even on non-cgroup kernels.

This fix is correct because it makes the SCHED_IDLE task-level policy behave as a **relative** policy within its own cgroup (like nice values), while the cgroup-level idle status (`cpu.idle=1`) determines the absolute priority between cgroups. The hierarchy walk ensures the comparison is always made at the appropriate level.

## Triggering Conditions

The bug requires the following conditions:

- **CONFIG_FAIR_GROUP_SCHED enabled**: The kernel must be built with cgroup CPU controller support. This is standard in virtually all distribution kernels.
- **Two cgroups**: One normal cgroup (default `cpu.idle=0`) and one idle cgroup (`cpu.idle=1`), both children of the root cgroup.
- **Mixed task-level and cgroup-level SCHED_IDLE**: A task with `SCHED_IDLE` per-task policy in the normal cgroup, and a task with `SCHED_NORMAL` per-task policy in the idle cgroup. This asymmetry between task policy and cgroup idle status is what triggers the bug.
- **Same CPU**: Both tasks must be runnable on the same CPU so they compete for wakeup preemption. Pinning both to the same CPU guarantees this.
- **Wakeup event**: One task must wake up while the other is currently running. This invokes `check_preempt_wakeup_fair()`, where the buggy logic resides.

The bug is **deterministic** — it triggers every time the conditions are met. There is no race condition or timing sensitivity. Any wakeup of task_A (SCHED_IDLE in normal_cgroup) while task_B (SCHED_NORMAL in idle_cgroup) is running will fail to preempt. Any wakeup of task_B while task_A is running will incorrectly preempt. The preemption decision is immediate and synchronous within the wakeup path.

There is no minimum CPU count requirement beyond having at least 2 CPUs (CPU 0 is reserved by kSTEP). No special topology, NUMA, or capacity asymmetry is needed. The kernel version must be v5.15 or later (when cgroup SCHED_IDLE support was added) and before v6.12-rc1 (when the fix was merged).

## Reproduce Strategy (kSTEP)

This bug can be reliably reproduced with kSTEP. The strategy involves creating two cgroups (one normal, one idle), placing tasks with opposing per-task policies into them, pinning both to the same CPU, and observing incorrect wakeup preemption behavior.

### Setup

1. **CPU configuration**: Use at least 2 CPUs in QEMU. CPU 0 is reserved for the driver. Pin both tasks to CPU 1.

2. **Cgroup configuration**:
   - Create `normal_cgroup` with default settings (`cpu.idle=0`).
   - Create `idle_cgroup` and set it to idle: `kstep_cgroup_write("idle_cgroup", "cpu.idle", "1")`.

3. **Task creation**:
   - Create `task_A` and set it to `SCHED_IDLE` per-task policy using `sched_setattr_nocheck()` with `{.sched_policy = SCHED_IDLE}`. Add it to `normal_cgroup`.
   - Create `task_B` as default `SCHED_NORMAL`. Add it to `idle_cgroup`.
   - Pin both tasks to CPU 1 with `kstep_task_pin(p, 1, 1)`.

### Triggering sequence

4. **Warm up both tasks**: Wake both tasks and run a few ticks so they are both in the run queue and have established vruntime values. Then pause task_A (so only task_B runs).

5. **Test wakeup preemption** (the key test):
   - Let task_B run for a few ticks (it becomes `curr` on CPU 1).
   - Wake task_A with `kstep_task_wakeup(task_A)`. This triggers `check_preempt_wakeup_fair()`.
   - After the wakeup, run one tick and check which task is currently running on CPU 1 using `cpu_rq(1)->curr`.

### Detection criteria

6. **Check preemption result**:
   - **Expected behavior (fixed kernel)**: task_A should preempt task_B because task_A's cgroup (normal_cgroup) has higher priority than task_B's cgroup (idle_cgroup). After the wakeup and a tick, `cpu_rq(1)->curr` should be task_A.
   - **Buggy behavior**: task_A does NOT preempt task_B because the buggy code sees `p->policy == SCHED_IDLE != SCHED_NORMAL` and returns early without examining the cgroup hierarchy. After the wakeup and a tick, `cpu_rq(1)->curr` remains task_B.
   - Use `kstep_pass()` / `kstep_fail()` based on whether the current task is task_A or task_B.

7. **Second test (reverse direction)**: Also test the reverse: let task_A run as `curr`, then wake task_B. On the buggy kernel, task_B will incorrectly preempt task_A (because `task_has_idle_policy(curr=A)` is true). On the fixed kernel, task_B should not preempt because its cgroup is idle.

### Observing TIF_NEED_RESCHED

8. **Alternative detection**: Instead of waiting for a tick to see task switching, you can check `test_tsk_need_resched(cpu_rq(1)->curr)` immediately after the wakeup call. On the fixed kernel, waking task_A should set `TIF_NEED_RESCHED` on task_B (preemption is requested). On the buggy kernel, `TIF_NEED_RESCHED` will NOT be set because the early return prevents reaching `resched_curr(rq)`.

### kSTEP API usage

- `kstep_cgroup_create("normal_cgroup")` and `kstep_cgroup_create("idle_cgroup")`
- `kstep_cgroup_write("idle_cgroup", "cpu.idle", "1")` to mark idle
- `kstep_task_create()` for both tasks
- `sched_setattr_nocheck(task_A, &(struct sched_attr){.sched_policy = SCHED_IDLE})` to set SCHED_IDLE
- `kstep_cgroup_add_task("normal_cgroup", task_A->pid)`
- `kstep_cgroup_add_task("idle_cgroup", task_B->pid)`
- `kstep_task_pin(task_A, 1, 1)` and `kstep_task_pin(task_B, 1, 1)`
- `kstep_task_wakeup()` to trigger the wakeup preemption path
- `cpu_rq(1)->curr` (via `KSYM_IMPORT` or `internal.h`) to observe which task is running
- `on_tick_begin` callback to check the currently running task after a wakeup

### No kSTEP framework modifications needed

SCHED_IDLE can be set via `sched_setattr_nocheck()` which is accessible from kernel module context (as demonstrated by the existing `kstep_task_fifo()` and `kstep_task_cfs()` implementations, and as explicitly used in the existing `eevdf_idle_entity_slice_protection.md` planned driver). The `cpu.idle` cgroup file can be written via `kstep_cgroup_write()`. All other required APIs are already available in kSTEP.
