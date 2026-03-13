# EEVDF: Spurious Reschedule from Wrong nr_running Check

**Commit:** `d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.12-rc4
**Buggy since:** v6.12-rc1 (introduced by commit `85e511df3cec` "sched/eevdf: Allow shorter slices to wakeup-preempt")

## Bug Description

The EEVDF scheduler's `update_curr()` function, which is called on every tick and at various scheduling decision points to update the current task's runtime statistics, contained an incorrect check that caused spurious rescheduling. When commit `85e511df3cec` refactored the deadline-expiry preemption logic from `update_deadline()` into `update_curr()`, it introduced a guard condition `if (rq->nr_running == 1) return;` to skip preemption checks when there is only one runnable task. However, this check used `rq->nr_running` (the total count of runnable tasks across all scheduling classes on the runqueue) instead of `cfs_rq->nr_running` (the count of runnable CFS tasks on the specific CFS runqueue).

This distinction matters because `rq->nr_running` includes tasks from all scheduling classes (RT, deadline, CFS, idle), while `cfs_rq->nr_running` only counts CFS tasks in the same hierarchy level. If there is a single CFS task running alongside an RT task on the same CPU, `rq->nr_running` would be 2, causing the guard to not trigger, and the code would proceed to call `resched_curr()` unnecessarily. The original code in `update_deadline()` (before the refactoring in `85e511df3cec`) used `cfs_rq->nr_running > 1`, which was the correct check.

More importantly, as discussed in the LKML thread, the more practical scenario involves CFS task group hierarchies. Consider a task p1 in a child cfs_rq (cfs_rq1) and a task p2 in a parent cfs_rq (cfs_rq2). Here `rq->nr_running` would be 2, but `cfs_rq1->nr_running` is only 1. Before the buggy commit, p1 would not be preempted by the deadline-expiry path because `cfs_rq->nr_running` was 1. After the buggy commit, since `rq->nr_running == 2`, the guard does not trigger and p1 gets marked for rescheduling via `resched_curr()`, leading to unnecessary context switches.

The kernel test robot (0day CI) detected this as a 13.1% regression in hackbench throughput on a 224-thread Intel Xeon Platinum 8480C system, with a 2.2% increase in involuntary context switches. The over-scheduling caused by the incorrect check significantly degraded throughput-sensitive workloads.

## Root Cause

The root cause lies in the `update_curr()` function in `kernel/sched/fair.c`. When commit `85e511df3cec` restructured the preemption logic, it moved the rescheduling decision out of `update_deadline()` and into `update_curr()`. The original `update_deadline()` code was:

```c
static void update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    if ((s64)(se->vruntime - se->deadline) < 0)
        return;
    /* ... update deadline ... */
    if (cfs_rq->nr_running > 1) {
        resched_curr(rq_of(cfs_rq));
        clear_buddies(cfs_rq, se);
    }
}
```

The refactored code changed `update_deadline()` to return a boolean `resched` flag, and moved the preemption decision to `update_curr()`:

```c
static void update_curr(struct cfs_rq *cfs_rq)
{
    /* ... update vruntime, deadline, min_vruntime ... */
    account_cfs_rq_runtime(cfs_rq, delta_exec);

    if (rq->nr_running == 1)    /* BUG: should be cfs_rq->nr_running */
        return;

    if (resched || did_preempt_short(cfs_rq, curr)) {
        resched_curr(rq);
        clear_buddies(cfs_rq, curr);
    }
}
```

The key error is that `rq->nr_running` counts all tasks on the runqueue regardless of scheduling class, while the preemption decision in `update_curr()` only concerns CFS tasks within the same `cfs_rq`. When `rq->nr_running` includes non-CFS tasks (RT, deadline) or CFS tasks in different hierarchy levels, the guard condition `rq->nr_running == 1` fails to skip the preemption path even when there is only one CFS task competing in the relevant cfs_rq.

The `resched` flag from `update_deadline()` is `true` when the current task has exhausted its time slice (its `vruntime` has passed its `deadline`). The `did_preempt_short()` function checks whether the current task, which previously benefited from short-slice preemption protection, has now become ineligible. Both of these checks are CFS-internal and should only trigger a reschedule when there are actually other CFS tasks on the same cfs_rq to switch to.

Peter Zijlstra (the maintainer) confirmed in the LKML thread that the use of `rq->nr_running` was unintended. He noted that if an RT task were present, `TIF_RESCHED` should already be set (from the RT wakeup preemption path), so the `resched_curr()` call would be a no-op in that specific case. The more impactful scenario is the CFS task group hierarchy case described by Chen Yu: one CFS task in a child cfs_rq and another CFS task in a parent cfs_rq, where `rq->nr_running` is 2 but `cfs_rq->nr_running` is 1 for the child.

## Consequence

The primary consequence is a significant performance regression for throughput-sensitive workloads. The kernel test robot measured a **13.1% throughput decrease** on hackbench running on a 224-thread, 2-socket Intel Xeon Platinum 8480C system with the following parameters: 50% threads, 4 iterations, process mode, socket IPC. Key metrics from the regression report include:

- `hackbench.throughput`: -13.1% (623219 → 541887)
- `hackbench.time.elapsed_time`: +16.3% (174.58s → 203.09s)
- `hackbench.time.involuntary_context_switches`: +2.2% (1.654e+08 → 1.69e+08)
- `vmstat.system.cs` (context switches): -15.8%
- `perf-stat.i.context-switches`: -15.9%
- `perf-stat.i.instructions`: -12.1%
- `perf-stat.i.ipc`: -12.2%

The over-scheduling causes tasks to be preempted unnecessarily, increasing context switch overhead and reducing useful CPU time. The CPI (cycles per instruction) increased by 14.3%, indicating more time wasted on context switching and cache misses rather than productive work. While this is a performance bug rather than a crash or data corruption issue, the throughput impact is severe for workloads that rely on tasks completing their time slices without interruption.

## Fix Summary

The fix is a one-line change in `update_curr()` in `kernel/sched/fair.c`, changing the guard condition from `rq->nr_running == 1` to `cfs_rq->nr_running == 1`:

```c
-	if (rq->nr_running == 1)
+	if (cfs_rq->nr_running == 1)
		return;
```

This restores the semantics of the original code in `update_deadline()` which checked `cfs_rq->nr_running > 1` before triggering a reschedule. By checking `cfs_rq->nr_running` instead of `rq->nr_running`, the code correctly skips the preemption checks when there are no other CFS tasks competing in the same CFS runqueue. This is the correct granularity because the EEVDF scheduling decisions (deadline expiry, short-slice preemption) are all scoped to a single `cfs_rq`.

The fix is correct and complete because it exactly matches the pre-refactoring behavior of `update_deadline()`. The `resched` and `did_preempt_short()` checks that follow only make sense when there is at least one other CFS entity in the same cfs_rq to preempt the current task in favor of. When `cfs_rq->nr_running == 1`, there is no other CFS task at the same hierarchy level, so any reschedule would be pointless for CFS scheduling purposes. K Prateek Nayak suggested this specific fix (using `cfs_rq->nr_running` rather than the v1 approach) and verified it restored hackbench performance.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **Multiple scheduling classes or CFS hierarchy levels**: The CPU must have `rq->nr_running >= 2` while `cfs_rq->nr_running == 1` for the cfs_rq of the current task. This happens when: (a) there is a CFS task running alongside an RT or deadline task on the same CPU, or (b) there are CFS tasks in different levels of a task group hierarchy (e.g., one task in a child cgroup's cfs_rq and another in the root cfs_rq).

2. **Deadline expiry or short-slice preemption**: The CFS task must have its `vruntime` exceed its `deadline` (i.e., `update_deadline()` returns `true`), or `did_preempt_short()` must return `true` (meaning the task previously benefited from short-slice preemption protection but is now ineligible). This happens naturally as a task consumes its time slice.

3. **The PREEMPT_SHORT feature must be enabled**: This is the default (`SCHED_FEAT(PREEMPT_SHORT, true)`).

4. **Kernel version**: The bug only exists between commits `85e511df3cec` (merged in v6.12-rc1) and `d4ac164bde7a` (merged in v6.12-rc4).

The bug is highly reproducible with workloads like hackbench that create many competing tasks. The regression is most visible on large multi-core systems with high thread counts. The scenario with CFS task group hierarchies (condition 1b) is the most practically impactful, as it causes unnecessary rescheduling of a lone CFS task in its cfs_rq simply because other CFS tasks exist at different hierarchy levels.

The bug does not require any special timing or race conditions. It triggers deterministically on every tick where the current CFS task's deadline expires, as long as the `rq->nr_running` vs `cfs_rq->nr_running` mismatch exists.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by creating a scenario where `rq->nr_running > 1` but `cfs_rq->nr_running == 1` on a given CPU, and then observing whether the CFS task gets spuriously rescheduled when its deadline expires.

### Approach 1: CFS task + RT task on the same CPU

1. **Setup**: Configure QEMU with at least 2 CPUs. Pin all test activity to CPU 1 (CPU 0 is reserved).

2. **Create tasks**:
   - Create one CFS task `p_cfs` using `kstep_task_create()` and pin it to CPU 1 with `kstep_task_pin(p_cfs, 1, 2)`.
   - Create one RT (FIFO) task `p_rt` using `kstep_task_create()` followed by `kstep_task_fifo(p_rt)` and pin it to CPU 1 with `kstep_task_pin(p_rt, 1, 2)`.

3. **Initial state**: Wake up `p_cfs` with `kstep_task_wakeup(p_cfs)`. The CFS task will be the current task on CPU 1. Keep `p_rt` paused initially.

4. **Create the mismatch**: Wake up `p_rt` with `kstep_task_wakeup(p_rt)`. Since RT has higher priority, `p_rt` will preempt `p_cfs` and `TIF_RESCHED` will be set. After `p_rt` runs, block it with `kstep_task_block(p_rt)`. Now `p_cfs` resumes. However, as Peter Zijlstra noted, with an RT task present, `TIF_RESCHED` would already be set, making `resched_curr()` a no-op. So Approach 2 is more reliable.

### Approach 2: CFS task group hierarchy (recommended)

This approach uses task groups to create the `rq->nr_running != cfs_rq->nr_running` mismatch, which is the more practically impactful scenario.

1. **Setup**: Configure QEMU with at least 2 CPUs.

2. **Create a cgroup**:
   - `kstep_cgroup_create("child")` to create a child cgroup.
   - `kstep_cgroup_set_cpuset("child", "1")` to restrict the cgroup to CPU 1.

3. **Create tasks**:
   - Create CFS task `p1` with `kstep_task_create()`, pin to CPU 1, and add to child cgroup: `kstep_cgroup_add_task("child", p1->pid)`.
   - Create CFS task `p2` with `kstep_task_create()`, pin to CPU 1, and keep in the root cgroup.

4. **Activate both tasks**: Wake up both `p1` and `p2` with `kstep_task_wakeup()`. Now on CPU 1: `rq->nr_running == 2`, but the child cgroup's `cfs_rq->nr_running == 1` (only `p1`), and the root `cfs_rq->nr_running == 2` (group SE for "child" + `p2`).

5. **Observe tick behavior**: Use the `on_tick_end` callback to inspect the scheduler state. After each tick:
   - Use `KSYM_IMPORT` to access `cpu_rq(1)` and read `rq->nr_running`, the child `cfs_rq->nr_running`, and whether `TIF_NEED_RESCHED` is set on the current task.
   - Track how many times `resched_curr()` is invoked by monitoring `TIF_NEED_RESCHED` transitions.

6. **Run ticks**: Use `kstep_tick_repeat(N)` for a sufficient number of ticks (e.g., 100) to let the tasks' deadlines expire multiple times.

7. **Detection criteria**:
   - **Buggy kernel**: When `p1` is running and is the sole task in its child cfs_rq (`cfs_rq->nr_running == 1`), `p1` will still get `TIF_NEED_RESCHED` set when its deadline expires because `rq->nr_running == 2`. Count the number of times the current CFS task on CPU 1 gets rescheduled (use `on_tick_end` to check if `TIF_NEED_RESCHED` is set). On the buggy kernel, this will be inflated.
   - **Fixed kernel**: When `p1` is running as the sole task in its child cfs_rq, it will NOT get `TIF_NEED_RESCHED` set from the deadline-expiry path because `cfs_rq->nr_running == 1`. The reschedule count will be lower.

8. **Pass/fail criterion**: Count the number of ticks where TIF_NEED_RESCHED is set when the current task is the sole task in its local cfs_rq. On the buggy kernel, this count will be non-zero; on the fixed kernel, it should be zero (or significantly lower). Use `kstep_pass()` / `kstep_fail()` accordingly.

### Alternative simpler approach: Direct state observation

A simpler variant that avoids cgroup complexity:

1. Create two CFS tasks `p1` and `p2`, both pinned to CPU 1.
2. Also create one RT task `p_rt` pinned to CPU 1, but keep it blocked.
3. Wake `p1` and `p2`. Let them run for several ticks, recording a baseline count of TIF_NEED_RESCHED events.
4. Now block `p2`, leaving only `p1` as a CFS task. Wake `p_rt` momentarily so it runs and then block it immediately.
5. Now `rq->nr_running` is 2 (p1 CFS + p_rt RT) but `cfs_rq->nr_running` is 1 (only p1).
6. Run ticks and observe: on the buggy kernel, `p1` will get spurious rescheduling when its deadline expires; on the fixed kernel it will not.

The recommended approach is Approach 2 (CFS hierarchy) as it directly demonstrates the real-world scenario that caused the hackbench regression, and avoids the complication Peter Zijlstra raised about RT tasks already having TIF_RESCHED set.
