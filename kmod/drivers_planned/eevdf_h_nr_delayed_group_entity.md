# EEVDF: Double-counting h_nr_delayed for group entity delayed dequeue

**Commit:** `3429dd57f0deb1a602c2624a1dd7c4c11b6c4734`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.14-rc2
**Buggy since:** v6.13-rc3 (introduced by `76f2f783294d` "sched/eevdf: More PELT vs DELAYED_DEQUEUE", though the LKML discussion notes the true root cause is `c2a295bffeaf` "sched/fair: Add new cfs_rq.h_nr_runnable" which reworked the accounting to use `h_nr_delayed`)

## Bug Description

The `set_delayed()` and `clear_delayed()` functions in the CFS scheduler's EEVDF delayed dequeue mechanism incorrectly adjust the `cfs_rq->h_nr_delayed` counter for the entire hierarchy whenever *any* sched_entity is delayed, regardless of whether that entity represents a task or a group (cfs_rq). The `h_nr_delayed` counter is intended to track the number of delayed *tasks* in the hierarchy so that the effective runnable count (`h_nr_running - h_nr_delayed`) accurately reflects truly runnable tasks for PELT load tracking. When a group entity is delayed, `set_delayed()` should not increment `h_nr_delayed` because the group entity does not represent an additional task—the task counts have already been adjusted by the `dequeue_entities()` loop.

The bug manifests in a cgroup hierarchy where a task blocks (triggering `dequeue_task_fair()`), and its parent group entity subsequently gets delayed because it is no longer eligible on the parent cfs_rq. In this scenario, both `dequeue_entities()` (which walks the hierarchy adjusting `h_nr_running`) and `set_delayed()` (which walks the hierarchy adjusting `h_nr_delayed`) end up decrementing the effective runnable count for the same logical event, causing a double-decrement.

The bug is described as "self-correcting" because cfs_rqs are per-CPU and cannot migrate. When the delayed group entity is eventually either fully dequeued (via `finish_delayed_dequeue_entity()` → `clear_delayed()`) or re-enqueued when a new task wakes up beneath it, `clear_delayed()` symmetrically over-decrements `h_nr_delayed`, restoring the correct value. However, during the interim period, the incorrect `h_nr_delayed` value influences PELT calculations through `se->runnable_weight` (set via `se_update_runnable()`) and the cfs_rq runnable average (computed in `__update_load_avg_cfs_rq()`).

The bug was discovered by K Prateek Nayak at AMD by adding a `SCHED_WARN_ON()` check after the `h_nr_runnable` update in `dequeue_entities()`, which was consistently tripped when running wakeup-intensive workloads like hackbench inside a cgroup.

## Root Cause

The root cause lies in the `set_delayed()` and `clear_delayed()` functions in `kernel/sched/fair.c` (lines 5372-5395 in the buggy version). These functions unconditionally walk the sched_entity hierarchy and adjust `h_nr_delayed` at each level:

```c
static void set_delayed(struct sched_entity *se)
{
    se->sched_delayed = 1;
    for_each_sched_entity(se) {
        struct cfs_rq *cfs_rq = cfs_rq_of(se);
        cfs_rq->h_nr_delayed++;
        if (cfs_rq_throttled(cfs_rq))
            break;
    }
}
```

The `h_nr_delayed` counter, together with `h_nr_running`, is used to compute the effective runnable count: `h_nr_running - h_nr_delayed`. This derived value feeds into PELT via two paths:
1. `se_update_runnable()` (sched.h line 910): `se->runnable_weight = cfs_rq->h_nr_running - cfs_rq->h_nr_delayed;`
2. `__update_load_avg_cfs_rq()` (pelt.c line 324): passes `cfs_rq->h_nr_running - cfs_rq->h_nr_delayed` as the runnable parameter.

Consider the scenario from the commit message:

```
        root
       /    \
      A      B (*) delayed since B is no longer eligible on root
      |      |
    Task0  Task1 <--- dequeue_task_fair() - task blocks
```

When Task1 blocks, `dequeue_task_fair()` calls `dequeue_entities(rq, &Task1->se, DEQUEUE_SLEEP)`. Inside `dequeue_entities()` (line 7015):

1. **h_nr_delayed initialization** (line 7033): Since `task_sleep` is true, the condition `!task_sleep && !task_delayed` is false, so `h_nr_delayed` is set to 0. This means the `dequeue_entities()` loops will NOT adjust `h_nr_delayed` for a sleeping task—that is expected since set_delayed/clear_delayed handle it.

2. **First loop iteration** (B's cfs_rq): `dequeue_entity(B_cfs_rq, Task1_se, DEQUEUE_SLEEP)` fully dequeues Task1 (returns true). The loop decrements `B_cfs_rq->h_nr_running -= 1` and `B_cfs_rq->h_nr_delayed -= 0`.

3. **B is now empty** (`cfs_rq->load.weight == 0`), so the loop continues upward to B's group entity on the root cfs_rq.

4. **Second loop iteration** (root cfs_rq): `dequeue_entity(root_cfs_rq, B_group_se, flags)` is called. Since B's group entity is sleeping (the sleep flag propagated) and NOT eligible on root, the DELAY_DEQUEUE path triggers: `set_delayed(B_group_se)` is called and `dequeue_entity` returns false.

5. **Inside `set_delayed(B_group_se)`**: The function sets `B_group_se->sched_delayed = 1`, then walks the hierarchy: `root_cfs_rq->h_nr_delayed++`. This increments h_nr_delayed by 1 at the root level.

6. **Back in `dequeue_entities()`**: Since `dequeue_entity` returned false, and `&p->se != B_group_se`, the first loop breaks. The second `for_each_sched_entity` loop continues from B_group_se, performing: `root_cfs_rq->h_nr_running -= 1` and `root_cfs_rq->h_nr_delayed -= 0`.

The final state at the root cfs_rq (assuming initial state of `h_nr_running=2, h_nr_delayed=0`):
- `h_nr_running = 2 - 1 = 1` (correct: only Task0 remains queued)
- `h_nr_delayed = 0 + 1 = 1` (INCORRECT: B is delayed but it's a group entity, not a task)
- Effective runnable = `1 - 1 = 0` (WRONG: should be 1, since Task0 is still running)

The problem is that `set_delayed()` does not distinguish between task entities and group entities. When a group entity becomes delayed, it should NOT increment `h_nr_delayed` because the task counts (`h_nr_running`) already properly account for the task departure via the `dequeue_entities()` loop. The `h_nr_delayed` counter is only needed for delayed *tasks*, where `dequeue_entities()` returns early (returns -1) and skips the `h_nr_running` adjustment loops.

## Consequence

The primary consequence is **incorrect PELT (Per-Entity Load Tracking) calculations**. The `h_nr_running - h_nr_delayed` value feeds into two critical PELT paths:

1. **`se_update_runnable()`**: Sets `se->runnable_weight` for group entities, which determines the runnable load contribution of a task group in load balancing decisions. When `h_nr_delayed` is over-counted, `runnable_weight` becomes artificially low (potentially zero or even appearing negative when cast to signed), causing the scheduler to underestimate the load on a CPU. This can lead to suboptimal load balancing decisions—the scheduler may not migrate tasks away from an overloaded CPU because it believes the CPU has fewer runnable tasks than it actually does.

2. **`__update_load_avg_cfs_rq()`**: The runnable component of the cfs_rq's PELT signal uses `h_nr_running - h_nr_delayed`. An over-counted `h_nr_delayed` suppresses the runnable average, which in turn affects CPU frequency selection via schedutil (the CPU may run at a lower frequency than warranted) and load balancing decisions across the system.

While the commit describes the error as "self-correcting" (since the entity will eventually be fully dequeued or re-enqueued, at which point `clear_delayed()` symmetrically over-decrements to restore correctness), the incorrect intermediate values persist for the duration of the delayed dequeue period. During wakeup-intensive workloads like hackbench running inside cgroups, this condition is triggered repeatedly and frequently, meaning the PELT signals are consistently inaccurate. Vincent Guittot confirmed in the LKML discussion that even during boot, a SCHED_WARN_ON checking `h_nr_queued == h_nr_runnable + h_nr_delayed` was triggered without the fix. The bug does not cause crashes or hangs, but it degrades scheduling quality through incorrect load estimates, potentially resulting in measurable performance regressions for latency-sensitive or throughput-sensitive workloads.

## Fix Summary

The fix adds an early return in both `set_delayed()` and `clear_delayed()` when the entity being delayed is NOT a task (i.e., it is a group entity corresponding to a cfs_rq):

```c
static void set_delayed(struct sched_entity *se)
{
    se->sched_delayed = 1;

    /* Delayed se of cfs_rq have no tasks queued on them.
     * Do not adjust h_nr_runnable since dequeue_entities()
     * will account it for blocked tasks. */
    if (!entity_is_task(se))
        return;

    for_each_sched_entity(se) {
        struct cfs_rq *cfs_rq = cfs_rq_of(se);
        cfs_rq->h_nr_delayed++;
        if (cfs_rq_throttled(cfs_rq))
            break;
    }
}
```

The same pattern is applied to `clear_delayed()`. The `sched_delayed` flag is still set/cleared unconditionally for all entity types (the early return is placed after setting the flag), but the `h_nr_delayed` hierarchy walk is now restricted to task entities only.

This fix is correct because the two categories of delayed entities have different accounting needs:
- **Delayed tasks**: When a task entity is delayed, `dequeue_entities()` returns -1 early (the `if (p && &p->se == se) return -1;` check), skipping the `h_nr_running` / `h_nr_delayed` adjustment loops entirely. Therefore, `set_delayed()` must adjust `h_nr_delayed` for the hierarchy, and the corresponding `clear_delayed()` (called during full dequeue or re-enqueue) will undo it.
- **Delayed group entities**: When a group entity is delayed, `dequeue_entities()` breaks out of the first loop and continues in the second loop, which correctly adjusts `h_nr_running` for the hierarchy. The `h_nr_delayed` adjustment is not needed because no *additional* task has been delayed—the task was already fully dequeued at a lower level.

## Triggering Conditions

The following conditions are needed to trigger the bug:

1. **Cgroup hierarchy**: At least two task groups (cgroups A and B) must exist under the root cfs_rq on the same CPU. This is necessary to have group sched_entities that can become delayed independently of their child tasks.

2. **DELAY_DEQUEUE feature enabled**: The `sched_feat(DELAY_DEQUEUE)` must be enabled (it is enabled by default in kernels v6.12+). This feature causes ineligible entities to be delayed rather than immediately dequeued.

3. **Task blocking in a cgroup**: A task (Task1) in cgroup B must block (sleep), triggering `dequeue_task_fair()` with `DEQUEUE_SLEEP` flags. The task must be the last (or only) task in cgroup B on that CPU, so that B's cfs_rq becomes empty and the dequeue propagates upward to B's group entity.

4. **Group entity ineligibility**: B's group sched_entity must be ineligible on the parent cfs_rq (root) at the time of dequeue. This occurs when B's virtual runtime (`vruntime`) exceeds the average vruntime (`avg_vruntime`) of the parent cfs_rq. In practice, this happens when cgroup B has consumed more than its fair share of CPU time relative to other entities on the same cfs_rq.

5. **Kernel version**: The bug exists in kernels that include commit `76f2f783294d` (v6.13-rc3) through the fix at `3429dd57f0de` (v6.14-rc2). Specifically, the `h_nr_delayed` tracking mechanism must be present.

The bug is highly reproducible with wakeup-intensive workloads. The commit author notes that running hackbench in a cgroup "consistently" triggers the SCHED_WARN_ON. The reason is that hackbench generates many rapid wakeup/sleep cycles, and with multiple cgroups, group entities frequently become ineligible and get delayed. Each such event causes a transient `h_nr_delayed` over-count.

No special hardware, NUMA topology, or number of CPUs is required beyond having at least one CPU available for the cgroup workload. The bug is deterministic in the sense that any time a group entity becomes delayed, the h_nr_delayed will be incorrectly incremented; there is no race condition involved.

## Reproduce Strategy (kSTEP)

The kSTEP framework can reproduce this bug because it supports cgroup creation, task management, blocking tasks, and direct access to internal scheduler state via `kernel/sched/sched.h` internals.

### Step-by-step plan:

1. **Topology setup**: Use at least 2 CPUs (QEMU default is fine). CPU 0 is reserved for the driver; tasks will be pinned to CPU 1.

2. **Cgroup creation**: Create two cgroups "A" and "B" using `kstep_cgroup_create("A")` and `kstep_cgroup_create("B")`. Optionally set cpuset for both cgroups to CPU 1 using `kstep_cgroup_set_cpuset()`.

3. **Task creation**: Create two tasks, Task0 and Task1. Pin both to CPU 1 using `kstep_task_pin(p, 1, 2)`. Assign Task0 to cgroup A via `kstep_cgroup_add_task("A", Task0->pid)` and Task1 to cgroup B via `kstep_cgroup_add_task("B", Task1->pid)`.

4. **Build up B's vruntime**: Let both tasks run for several ticks using `kstep_tick_repeat()`. Then pause Task0 (`kstep_task_pause(Task0)`) so that only Task1 runs. Advance several more ticks so Task1 / cgroup B consumes significant vruntime, making B's group entity ineligible on the root cfs_rq. Resume Task0 afterward with `kstep_task_wakeup(Task0)`.

5. **Trigger the bug**: With both tasks running, record the current `h_nr_delayed` value from `cpu_rq(1)->cfs.h_nr_delayed` (the root cfs_rq on CPU 1). Then block Task1 using `kstep_task_block(Task1)`. This triggers `dequeue_task_fair()` → `dequeue_entities()` → `set_delayed(B_group_se)`.

6. **Advance a tick**: Call `kstep_tick()` to allow the scheduler to process the dequeue and update PELT. This ensures `se_update_runnable()` and `__update_load_avg_cfs_rq()` are called with the (potentially incorrect) `h_nr_delayed` value.

7. **Observe the bug**: Read `cpu_rq(1)->cfs.h_nr_delayed` after the block. On the **buggy kernel**, this value will be 1 (because `set_delayed()` incorrectly incremented it for B's group entity). On the **fixed kernel**, this value will be 0 (because `set_delayed()` returns early for non-task entities).

8. **Detection logic**: Also verify B's group entity's delayed status by accessing B's task group's `se[1]` (the sched_entity for CPU 1) and checking `se->sched_delayed == 1`. Then check:
   - `root_cfs_rq->h_nr_running` should be 1 (only Task0 is queued in the hierarchy)
   - On buggy: `root_cfs_rq->h_nr_delayed` == 1 → effective runnable = 0 (WRONG)
   - On fixed: `root_cfs_rq->h_nr_delayed` == 0 → effective runnable = 1 (CORRECT)

9. **Alternative detection via PELT**: After blocking Task1 and ticking, check `se_runnable()` for B's parent group entity or directly read `root_cfs_rq->h_nr_running - root_cfs_rq->h_nr_delayed`. If this value is 0 when there is still a runnable task (Task0), the bug is confirmed.

10. **Pass/fail criteria**: Use `kstep_pass()` if `h_nr_delayed == 0` after blocking Task1 (fixed behavior). Use `kstep_fail()` if `h_nr_delayed > 0` when no task entity is delayed (buggy behavior). Specifically, check: `h_nr_delayed > 0 && no task in the hierarchy has sched_delayed set`.

### Making B's group entity ineligible:

The key challenge is ensuring B's group entity is ineligible on the root cfs_rq when Task1 blocks. Ineligibility in EEVDF means `entity_eligible(root_cfs_rq, B_group_se)` returns false, which happens when B's vruntime is ahead of the average. To achieve this:

- Give cgroup A a higher weight (e.g., `kstep_cgroup_set_weight("A", 1000)`) and cgroup B a lower weight (e.g., `kstep_cgroup_set_weight("B", 100)`). This means B accumulates vruntime faster per unit of execution.
- Let Task1 (in B) run for a while. Since B has lower weight, B's vruntime advances faster, making it ineligible sooner.
- Alternatively, pause Task0 temporarily, let Task1 run exclusively for many ticks, then wake Task0. This ensures B's group se has consumed substantial CPU time, pushing its vruntime well ahead of A's on the root cfs_rq.
- Use `kstep_eligible()` to verify that B's group se is ineligible before proceeding to block Task1. If it is still eligible, run more ticks until it becomes ineligible.

### Internal state access:

The driver needs to access:
- `cpu_rq(1)->cfs` — the root cfs_rq on CPU 1
- `cpu_rq(1)->cfs.h_nr_delayed` — the counter being buggy
- `cpu_rq(1)->cfs.h_nr_running` — to compute effective runnable
- B's task group sched_entity: obtained via the task_group structure and its per-cpu `se[]` array

All of these are accessible via kSTEP's `internal.h` which includes `kernel/sched/sched.h`.
