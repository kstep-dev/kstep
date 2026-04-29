# Bandwidth: Throttled cfs_rq Skips h_nr_running and load_avg Updates on Enqueue/Dequeue

**Commit:** `5ab297bab984310267734dfbcc8104566658ebef`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.7-rc1
**Buggy since:** v5.7-rc1 (introduced by `6d4d22468dae` "sched/fair: Reorder enqueue/dequeue_task_fair path", which was also merged in v5.7-rc1; both the bug introduction and fix landed in the same release cycle, between v5.6-rc3 and v5.7-rc1)

## Bug Description

The `enqueue_task_fair()` and `dequeue_task_fair()` functions walk the CFS hierarchy in two distinct `for_each_sched_entity()` loops when enqueuing or dequeuing a task. The first loop handles entities that are not yet on the runqueue (`!se->on_rq`), calling `enqueue_entity()` or `dequeue_entity()` and updating `h_nr_running`. When the first loop terminates (either because it found an already-enqueued ancestor, or the cfs_rq became empty during dequeue), the second loop walks the remaining ancestor entities to update their load averages (`update_load_avg()`), runnable state (`se_update_runnable()`), group weight (`update_cfs_group()`), and `h_nr_running` counters.

Commit `6d4d22468dae` reordered the second loop so that the throttled cfs_rq check (`cfs_rq_throttled()`) was performed **before** the `update_load_avg()` and `h_nr_running` updates. The intent was to move the throttle check to the top of the loop for code clarity, since "everything related to a cfs_rq (throttled or not) will be done in the same loop." However, this reordering introduced a bug: when a child cgroup's group scheduling entity (`gse`) is already enqueued on a throttled parent cfs_rq, the second loop would immediately hit the throttled check and jump to `enqueue_throttle`/`dequeue_throttle` **without** performing the `update_load_avg()` call or updating `h_nr_running` for the throttled cfs_rq.

The scenario arises because even when a cgroup is throttled, the group scheduling entity of a child cgroup can remain enqueued (`gse->on_rq == true`). When a new task is enqueued in such a child cgroup, the first loop breaks immediately (because `gse->on_rq` is true), and the second loop starts walking from that group entity upward. If the parent cfs_rq is throttled, the buggy code skips the load_avg update and `h_nr_running` increment for that throttled cfs_rq before jumping out.

The same problem exists in `dequeue_task_fair()`, where during dequeue, the scheduling entity may be moved to a parent entity before breaking out of the first loop, and the second loop similarly skips the throttled cfs_rq's updates.

## Root Cause

The root cause is incorrect ordering of the `cfs_rq_throttled()` check relative to the `update_load_avg()` and `h_nr_running` updates in the second `for_each_sched_entity()` loop of both `enqueue_task_fair()` and `dequeue_task_fair()`.

In the buggy code introduced by `6d4d22468dae`, the second loop in `enqueue_task_fair()` looked like:

```c
for_each_sched_entity(se) {
    cfs_rq = cfs_rq_of(se);

    /* end evaluation on encountering a throttled cfs_rq */
    if (cfs_rq_throttled(cfs_rq))
        goto enqueue_throttle;

    update_load_avg(cfs_rq, se, UPDATE_TG);
    update_cfs_group(se);

    cfs_rq->h_nr_running++;
    cfs_rq->idle_h_nr_running += idle_h_nr_running;
}
```

The problem is that the throttle check at the top causes the loop to skip `update_load_avg()`, `se_update_runnable()`, `update_cfs_group()`, and the `h_nr_running++` for the throttled cfs_rq itself. Even though the cfs_rq is throttled, these updates are still necessary because: (a) `h_nr_running` must be kept consistent to accurately reflect how many tasks are hierarchically runnable below each cfs_rq, and (b) `update_load_avg()` needs to be called to sync the PELT load tracking up to the point of throttling.

The specific problematic scenario unfolds as follows:

1. A cgroup hierarchy exists: root → parent_cg → child_cg.
2. parent_cg's cfs_rq is throttled (CFS bandwidth control has exhausted its quota).
3. child_cg's group scheduling entity (`gse`) is already enqueued on parent_cg's cfs_rq (`gse->on_rq == true`), because throttling the parent does not dequeue the child group entities.
4. A new task T is enqueued in child_cg.
5. In `enqueue_task_fair()`, the first `for_each_sched_entity()` loop starts with `se = &T->se`. It enqueues T into child_cg's cfs_rq, increments `child_cfs_rq->h_nr_running`, and then moves to `se = gse` (the group entity of child_cg). Since `gse->on_rq == true`, it breaks immediately.
6. The second loop starts iterating from `gse`. It gets `cfs_rq = parent_cfs_rq` (the throttled cfs_rq). The buggy throttle check fires immediately, jumping to `enqueue_throttle` without updating `parent_cfs_rq->h_nr_running` or calling `update_load_avg()` on parent_cfs_rq.

The identical issue exists in `dequeue_task_fair()` where the dequeue path's second loop also places the throttle check before the updates.

## Consequence

The primary consequence is **incorrect `h_nr_running` accounting** on throttled cfs_rq structures. The `h_nr_running` field tracks the total number of tasks hierarchically below a given cfs_rq. When tasks are enqueued or dequeued in a child of a throttled cgroup, the throttled cfs_rq's `h_nr_running` is not updated, leading to a stale/incorrect count.

This manifests as incorrect PELT (Per-Entity Load Tracking) load average calculations. The commit message notes: "we have to update both load_avg with the old h_nr_running and increase h_nr_running in such case." The `update_load_avg()` function uses `h_nr_running` as part of its calculations (this was the motivation for the original reordering commit, which aimed to "enable the use of h_nr_running in PELT algorithm"). With the buggy ordering, the load average is not synced to the throttled time, potentially causing load tracking to be subtly wrong for the throttled hierarchy.

While the bug may not cause an immediate crash or obvious malfunction, it leads to inaccurate load tracking statistics. This can affect CFS bandwidth control decisions, load balancing decisions, and CPU frequency scaling decisions (since schedutil uses PELT signals). The impact is subtle but could cause scheduling anomalies in workloads with nested cgroups and CFS bandwidth control, such as container orchestration environments. The commit message notes that "the update of load_avg will effectively happen only once in order to sync up to the throttled time" — meaning the missed update causes a one-time desynchronization that persists until the next unthrottle event.

## Fix Summary

The fix in commit `5ab297bab984` moves the `cfs_rq_throttled()` check to **after** the `update_load_avg()`, `se_update_runnable()`, `update_cfs_group()`, and `h_nr_running` update operations in the second `for_each_sched_entity()` loop of both `enqueue_task_fair()` and `dequeue_task_fair()`.

In the fixed `enqueue_task_fair()`, the second loop becomes:

```c
for_each_sched_entity(se) {
    cfs_rq = cfs_rq_of(se);

    update_load_avg(cfs_rq, se, UPDATE_TG);
    se_update_runnable(se);
    update_cfs_group(se);

    cfs_rq->h_nr_running++;
    cfs_rq->idle_h_nr_running += idle_h_nr_running;

    /* end evaluation on encountering a throttled cfs_rq */
    if (cfs_rq_throttled(cfs_rq))
        goto enqueue_throttle;
}
```

And similarly in `dequeue_task_fair()`:

```c
for_each_sched_entity(se) {
    cfs_rq = cfs_rq_of(se);

    update_load_avg(cfs_rq, se, UPDATE_TG);
    se_update_runnable(se);
    update_cfs_group(se);

    cfs_rq->h_nr_running--;
    cfs_rq->idle_h_nr_running -= idle_h_nr_running;

    /* end evaluation on encountering a throttled cfs_rq */
    if (cfs_rq_throttled(cfs_rq))
        goto dequeue_throttle;
}
```

This ensures that even when a throttled cfs_rq is encountered, the load averages are updated and `h_nr_running` is adjusted for that cfs_rq before the loop terminates. The fix is correct because the throttled cfs_rq itself still needs its bookkeeping maintained even though iteration should not continue past it to unthrottled ancestors. The fix was reviewed by Ben Segall, who also reported that the dequeue path had the same issue (the v2 patch only fixed enqueue; v3 fixed both).

## Triggering Conditions

The bug requires the following specific conditions:

- **CFS bandwidth control must be enabled** (`CONFIG_CFS_BANDWIDTH=y`), as the bug only manifests when `cfs_rq_throttled()` can return true.
- **Nested cgroup hierarchy**: At least two levels of cgroup hierarchy are needed (e.g., root → parent → child), where the parent cgroup has CFS bandwidth limits configured (`cpu.cfs_quota_us` and `cpu.cfs_period_us`).
- **The parent cgroup must be throttled**: The parent cgroup must have exhausted its CFS bandwidth quota, causing its cfs_rq to be throttled.
- **Child group entity must remain enqueued**: When the parent cgroup is throttled, the child cgroup's group scheduling entity (`gse`) must still have `on_rq == true`. This happens naturally because throttling a parent cfs_rq does not dequeue child group entities from it.
- **Task enqueue/dequeue in the child cgroup**: A task must be enqueued (e.g., woken up) or dequeued (e.g., goes to sleep) in the child cgroup while the parent is throttled. This causes the first `for_each_sched_entity()` loop to break early (because `gse->on_rq` is already true for enqueue, or because the child cfs_rq still has other tasks for dequeue), and the second loop encounters the throttled parent cfs_rq.

The bug is **deterministic** once the above conditions are met — there is no race condition or timing dependency beyond ensuring the parent cgroup is throttled when the task enqueue/dequeue occurs. Any number of CPUs can trigger this, as the bug is per-CPU and depends only on the cgroup hierarchy state.

The probability of hitting this in practice is moderate in environments using CFS bandwidth control with nested cgroups (e.g., Kubernetes pods with CPU limits, where the pod cgroup and container cgroups form a nested hierarchy).

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP because the affected kernel version is too old.

1. **WHY can this bug not be reproduced with kSTEP?** The bug was introduced by commit `6d4d22468dae` (after v5.6-rc3) and fixed by commit `5ab297bab984310267734dfbcc8104566658ebef` (after v5.6-rc4). Both commits were merged into v5.7-rc1. kSTEP supports Linux v5.15 and newer only. The kernel version containing this bug (v5.6-rc3 to v5.7-rc1 development window) is significantly older than v5.15, making it incompatible with the kSTEP framework.

2. **WHAT would need to be added to kSTEP to support this?** No kSTEP changes are needed — the issue is purely a kernel version compatibility constraint. If the kernel version were supported, kSTEP already has the necessary capabilities to reproduce this bug: it can create cgroup hierarchies (`kstep_cgroup_create()`), set CFS bandwidth limits (via `kstep_sysctl_write()` or cgroup filesystem writes), create and enqueue/dequeue tasks (`kstep_task_create()`, `kstep_task_block()`, `kstep_task_wakeup()`), and observe internal scheduler state (`h_nr_running`, load_avg) via `KSYM_IMPORT()` and direct cfs_rq access through `internal.h`.

3. **Kernel version is pre-v5.15**: The fix targets the v5.6/v5.7 development cycle, which is approximately 18 months before v5.15. kSTEP explicitly supports v5.15+ only.

4. **Alternative reproduction methods**: To reproduce this bug outside kSTEP:
   - Check out the kernel at `6d4d22468dae` (the buggy commit) and build it with `CONFIG_CFS_BANDWIDTH=y` and `CONFIG_FAIR_GROUP_SCHED=y`.
   - Create a nested cgroup hierarchy: `/sys/fs/cgroup/cpu/parent/child/`.
   - Set a tight CFS bandwidth quota on the parent cgroup (e.g., 10ms quota, 100ms period).
   - Run a CPU-intensive workload in the parent cgroup to exhaust the bandwidth quota and trigger throttling.
   - While the parent is throttled, wake up or enqueue a task in the child cgroup.
   - Observe that `h_nr_running` on the parent's cfs_rq does not reflect the newly enqueued task using tracepoints or ftrace.
   - Add `printk()` instrumentation in the second `for_each_sched_entity()` loop of `enqueue_task_fair()` to verify that `update_load_avg()` and `h_nr_running++` are being skipped for the throttled cfs_rq.
