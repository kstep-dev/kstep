# PELT: Missing Load Decay Due to cfs_rq Not on Leaf List

**Commit:** `0258bdfaff5bd13c4d2383150b7097aecd6b6d82`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.13-rc1
**Buggy since:** v4.8-rc1 (3d30544f0212 "sched/fair: Apply more PELT fixes"), worsened by v5.1-rc1 (039ae8bcf7a5 "sched/fair: Fix O(nr_cgroups) in the load balancing path")

## Bug Description

When a sleeping (idle) task is attached to a cgroup's `cfs_rq` via `attach_entity_cfs_rq()`, the task's PELT load averages are added to the new `cfs_rq`. If the task is subsequently moved to a different CPU before it is ever enqueued (for example, by applying a cpuset that forces migration), the sched_entity's load is transferred into `cfs_rq->removed` via the migration path. However, the corresponding `cfs_rq` is never added to the per-CPU `leaf_cfs_rq_list`, which means `update_blocked_averages()` — the periodic function that decays stale PELT load — never processes this `cfs_rq`. As a result, the phantom load remains permanently on the `task_group`'s `tg_load_avg`, skewing the weight calculation for all sibling cfs_rqs in the hierarchy.

This bug is particularly severe in containerized environments because virtually all container runtimes (Docker, crun, runc, containerd) follow a pattern of: (1) fork a child process, (2) attach it to a cgroup hierarchy, (3) apply cpuset constraints, and (4) exec the target program. Steps 2 and 3 happen while the child is still idle/sleeping, which is precisely the sequence that triggers this bug. The issue was confirmed to affect real production workloads and was reproduced with both cgroup v1 and cgroup v2.

The severity of the fairness violation depends on the cgroup hierarchy depth and the relative weights of the task groups. In a simple two-cgroup hierarchy with equal weights, the skew is typically 60/40 to 70/30. In deeper hierarchies with asymmetric weights in sub-groups, the skew can reach 1/99 — meaning one cgroup gets 99% of CPU time while its equally-weighted sibling gets only 1%.

The root cause has two related facets, corresponding to two separate commits referenced in the `Fixes:` tags. The first (3d30544f0212, v4.8) introduced a path where the cfs_rq was never added to the leaf list when it was the first entity attached to a fresh cfs_rq. The second (039ae8bcf7a5, v5.1) made the problem worse by actively removing cfs_rqs with zero PELT load from the leaf list, meaning even cfs_rqs that were once visible could become invisible to `update_blocked_averages()` if they happened to have zero load at the time of removal.

## Root Cause

The function `propagate_entity_cfs_rq()` is called from both `attach_entity_cfs_rq()` and `detach_entity_cfs_rq()`. Its purpose is to propagate PELT load changes up the task group hierarchy after a sched_entity is attached to or detached from a cfs_rq. Before the fix, the function looked like:

```c
static void propagate_entity_cfs_rq(struct sched_entity *se)
{
    struct cfs_rq *cfs_rq;

    /* Start to propagate at parent */
    se = se->parent;

    for_each_sched_entity(se) {
        cfs_rq = cfs_rq_of(se);

        if (cfs_rq_throttled(cfs_rq))
            break;

        update_load_avg(cfs_rq, se, UPDATE_TG);
    }
}
```

The critical omission is that this function never calls `list_add_leaf_cfs_rq()` on the cfs_rq that the sched_entity belongs to (i.e., `cfs_rq_of(se)` for the original `se` before we start at `se->parent`). The leaf cfs_rq list is the mechanism by which `update_blocked_averages()` discovers which cfs_rqs need periodic load decay processing. If a cfs_rq is not on this list, its `removed` load (load that was transferred there during a migration) will never be subtracted from the task group's `tg_load_avg`.

The specific failure sequence is:
1. A sleeping task `T` is attached to cgroup `cg-1` which has a `cfs_rq` on CPU 0. `attach_entity_cfs_rq()` is called, which calls `attach_entity_load_avg()` to add `T`'s load to `cfs_rq->avg` and then calls `propagate_entity_cfs_rq()` to update parent `tg_load_avg` values.
2. Before `T` wakes up, a cpuset change forces `T`'s allowed CPUs to, say, CPU 1. The migration path calls `remove_entity_load_avg()`, which atomically moves the sched_entity's load into `cfs_rq->removed.load_avg` and `cfs_rq->removed.util_avg` on the original CPU 0 cfs_rq.
3. Normally, `update_blocked_averages()` iterates over `leaf_cfs_rq_list` for each CPU and calls `update_cfs_rq_load_avg()` which checks `cfs_rq->removed` and subtracts the removed load from both `cfs_rq->avg` and `tg_load_avg`.
4. However, if the cfs_rq was never added to `leaf_cfs_rq_list` (because no task was ever enqueued on it — step 1 only attached load, never enqueued), then `update_blocked_averages()` never visits this cfs_rq.
5. The stale load persists in `tg_load_avg` forever. Since `tg_load_avg` is the denominator in calculating each cfs_rq's share of the task group's weight (`calc_group_shares()`), the inflated value causes all other cfs_rqs in the same task group to receive a smaller share of the group's weight, thereby advancing their vruntimes faster than expected.

For the deeper hierarchy case (e.g., `parent/cg-1/sub` and `parent/cg-2/sub`), the phantom load can accumulate at multiple levels of the hierarchy, compounding the effect and leading to the extreme 1/99 fairness violations observed in practice.

## Consequence

The primary consequence is severe CPU time unfairness between cgroups. Two equally-weighted cgroups sharing the same CPU can end up with wildly asymmetric CPU time allocation, ranging from 60/40 in simple cases to 1/99 in worst cases with deeper cgroup hierarchies. This directly violates the fairness guarantees that CFS is supposed to provide through proportional fair sharing.

In real-world production environments, this manifests as container workloads receiving dramatically less CPU time than their configured cgroup weights would suggest. Since the phantom load persists indefinitely (it is never decayed), the unfairness is permanent for the lifetime of the cgroups. The issue is particularly insidious because `/proc/sched_debug` only prints cfs_rqs that are on the leaf list — the very cfs_rqs with the stale load are hidden from this diagnostic output, making the root cause extremely difficult to identify from userspace. The only way to detect it prior to the fix was using tracing tools like bpftrace, or by carefully inferring it from the mismatch between `tg_load_avg_contrib` and `tg_load_avg` in the visible cfs_rq entries.

There is no crash, hang, or data corruption. The impact is purely a scheduling fairness degradation. However, in environments with strict latency or throughput SLAs, this can cause real service-level violations. The author confirmed finding this on production servers with real container workloads.

## Fix Summary

The fix modifies `propagate_entity_cfs_rq()` to ensure that all cfs_rqs in the hierarchy are added to the `leaf_cfs_rq_list` so that `update_blocked_averages()` can later process them and properly decay any removed load. The key changes are:

1. **Add the initial cfs_rq to the leaf list**: Before starting the upward propagation at `se->parent`, the fix inserts `list_add_leaf_cfs_rq(cfs_rq_of(se))` to ensure the cfs_rq that the sched_entity directly belongs to is on the leaf list. This is the critical fix — it guarantees that even if no task is ever enqueued on this cfs_rq, it will still be visited by `update_blocked_averages()`.

2. **Add parent cfs_rqs to the leaf list during propagation**: In the `for_each_sched_entity` loop, for non-throttled cfs_rqs, the fix adds `list_add_leaf_cfs_rq(cfs_rq)` after `update_load_avg()`. For throttled cfs_rqs, it calls `list_add_leaf_cfs_rq(cfs_rq)` and only breaks if the cfs_rq was already on the list (i.e., `list_add_leaf_cfs_rq()` returned true, meaning it was already present). This ensures that throttled cfs_rqs that are not yet on the leaf list get added too, while avoiding unnecessary traversal if the ancestor chain is already fully represented.

This approach is correct because `list_add_leaf_cfs_rq()` is idempotent (adding an already-present cfs_rq is a no-op that returns true), and the only cost is ensuring visibility to `update_blocked_averages()`. Once on the leaf list, the normal blocked load decay mechanism takes care of eventually removing the stale load and, if the cfs_rq becomes completely empty, removing it from the leaf list again.

## Triggering Conditions

The bug requires the following precise conditions:

- **CONFIG_FAIR_GROUP_SCHED** must be enabled (this is the standard configuration for cgroup CPU scheduling).
- **At least 2 CPUs** are needed: one where the task is initially attached (and its load added to the cfs_rq), and another where the task ends up after the cpuset change.
- **A sleeping/idle task** must be attached to a cgroup's cfs_rq via `sched_move_task()` → `attach_entity_cfs_rq()`. This happens when writing a PID to a cgroup's `cgroup.procs` file while the task is sleeping.
- **The task must be migrated to a different CPU before being enqueued**, typically by applying a cpuset constraint (writing to `cpuset.cpus`) that excludes the CPU where the task was initially attached. This triggers `set_cpus_allowed_common()` → `remove_entity_load_avg()` which moves the load to `cfs_rq->removed`.
- **The cfs_rq must not already be on the leaf list**. This happens when it's a fresh cfs_rq with no prior load, or when it was previously removed from the leaf list due to having zero load (the optimization from commit 039ae8bcf7a5).
- **No other task should be subsequently enqueued on the same cfs_rq on the original CPU**, because enqueuing a task would add the cfs_rq to the leaf list, allowing the stale load to eventually be decayed.

The bug is highly reproducible because the container runtime startup sequence naturally creates exactly this condition: fork → attach to cgroup → apply cpuset → exec. The timing window is not tight; the child process is sleeping the entire time between fork and exec, providing a wide window for the cgroup and cpuset operations.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP.

1. **WHY it cannot be reproduced**: The fix was merged in **v5.13-rc1**, meaning all buggy kernels are v5.12 or older. kSTEP supports Linux v5.15 and newer only. There is no kernel version ≥ v5.15 that contains this bug (it was fixed well before v5.15), so kSTEP cannot check out a buggy kernel to test against.

2. **WHAT would need to change**: The only way to reproduce this in kSTEP would be to lower the minimum supported kernel version to at least v5.12 or earlier. This would require significant testing and validation of the kSTEP framework against older kernel APIs, sched internals, and build infrastructure that may have changed between v5.12 and v5.15.

3. **Version constraint**: This is a **kernel version too old** case. The fix targets v5.13-rc1, which is pre-v5.15. kSTEP's minimum supported version is v5.15.

4. **Alternative reproduction outside kSTEP**: The bug can be easily reproduced on any kernel between v4.8 and v5.12 using the scripts provided in the LKML cover letter. The simplest approach is:
   - Boot a kernel in the v4.8–v5.12 range with CONFIG_FAIR_GROUP_SCHED=y and at least 2 CPUs.
   - Create a cgroup hierarchy with two equally-weighted child groups.
   - For each child group: fork a child process, attach it to the cgroup while it's sleeping, apply a cpuset pinning it to a single shared CPU, then wake the child to run a CPU-intensive workload.
   - Observe the CPU time split via `ps u` or `/proc/[pid]/stat`. On a buggy kernel, the split will deviate significantly from 50/50 (often 60/40 or worse; with deeper hierarchies, 1/99 is possible). On a fixed kernel, the split will be approximately 50/50.
   - The LKML thread also includes a Docker-based reproduction: simply running `docker run --cpuset-cpus=0 --rm -it <stress-image>` twice is often sufficient to trigger the issue on affected kernels.

5. **If kSTEP were hypothetically extended to support v5.12**: The reproduction would be straightforward using existing kSTEP APIs. The driver would: (a) create two cgroups with equal weight using `kstep_cgroup_create()` and `kstep_cgroup_set_weight()`, (b) create two tasks with `kstep_task_create()` (which start paused/sleeping), (c) add each task to its respective cgroup with `kstep_cgroup_add_task()`, (d) apply a cpuset pinning both cgroups to CPU 1 with `kstep_cgroup_set_cpuset()`, (e) wake both tasks with `kstep_task_wakeup()`, (f) run many ticks with `kstep_tick_repeat()`, and (g) check `sum_exec_runtime` on both tasks' sched_entities to verify the CPU time split. On a buggy kernel, one task would receive significantly more CPU time than the other; on the fixed kernel, they would be approximately equal.
