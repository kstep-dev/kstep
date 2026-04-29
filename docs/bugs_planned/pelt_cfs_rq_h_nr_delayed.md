# PELT: cfs_rq runnable stats not adjusted for DELAYED_DEQUEUE entities

**Commit:** `76f2f783294d7d55c2564e2dfb0a7279ba0bc264`
**Affected files:** `kernel/sched/fair.c`, `kernel/sched/pelt.c`, `kernel/sched/sched.h`, `kernel/sched/debug.c`
**Fixed in:** v6.13-rc3
**Buggy since:** v6.12-rc1 (introduced by commit `fc1892becd56` "sched/eevdf: Fixup PELT vs DELAYED_DEQUEUE")

## Bug Description

The EEVDF scheduler's DELAY_DEQUEUE feature keeps sleeping `sched_entity` objects enqueued on the runqueue until their negative lag has elapsed. This is intentional for scheduling fairness — it prevents tasks from gaining an unfair advantage by sleeping and then being placed with favorable vruntime. However, while these delayed-dequeue entities remain on the runqueue, they should not be counted as truly runnable for the purposes of PELT (Per-Entity Load Tracking) statistics.

Commit `fc1892becd56` ("sched/eevdf: Fixup PELT vs DELAYED_DEQUEUE") partially addressed this by making `se_runnable()` return `false` for entities with `sched_delayed == 1`, which corrected the per-entity runnable tracking. However, it did not adjust the `cfs_rq`-level runnable statistics. Specifically, the `cfs_rq->h_nr_running` field — which counts the hierarchical number of running tasks in a CFS runqueue — continued to include delayed-dequeue entities. This field is used directly in `__update_load_avg_cfs_rq()` as the `runnable` parameter to `___update_load_sum()`, meaning the cfs_rq-level `runnable_avg` was inflated.

Additionally, for group scheduling (`CONFIG_FAIR_GROUP_SCHED`), the function `se_update_runnable()` set a group entity's `runnable_weight` to `se->my_q->h_nr_running`, which also included delayed entities. This caused the group entity's runnable weight to be too high, further propagating the inflated signal up the task group hierarchy and ultimately biasing load balancing decisions and cgroup share calculations.

## Root Cause

The root cause is that `cfs_rq->h_nr_running` includes both truly runnable tasks and tasks that are in delayed-dequeue state (i.e., `sched_delayed == 1`). There was no mechanism to track how many of the `h_nr_running` entities were actually delayed, so the cfs_rq-level PELT calculations could not discount them.

In `__update_load_avg_cfs_rq()` (in `kernel/sched/pelt.c`), the runnable signal was computed as:

```c
if (___update_load_sum(now, &cfs_rq->avg,
                       scale_load_down(cfs_rq->load.weight),
                       cfs_rq->h_nr_running,    /* <-- includes delayed entities */
                       cfs_rq->curr != NULL)) {
```

Since `h_nr_running` included delayed-dequeue tasks, the `runnable` parameter passed to `___update_load_sum()` was higher than the actual number of runnable tasks. Over time, this inflated `cfs_rq->avg.runnable_avg`.

Similarly, in `se_update_runnable()` (in `kernel/sched/sched.h`):

```c
static inline void se_update_runnable(struct sched_entity *se)
{
    if (!entity_is_task(se))
        se->runnable_weight = se->my_q->h_nr_running;  /* includes delayed */
}
```

This set the group entity's `runnable_weight` to a value that included delayed entities, making the parent cfs_rq believe the group had more runnable tasks than it actually did.

The prior fix (`fc1892becd56`) only addressed the per-entity level: `se_runnable()` was made to return `false` when `se->sched_delayed` is set. But the cfs_rq aggregate was left inconsistent. This meant there was a mismatch: `\Sum se->runnable_avg` (which correctly excluded delayed entities) did not match `cfs_rq->runnable_avg` (which incorrectly included them via `h_nr_running`).

The propagation through `throttle_cfs_rq()`, `unthrottle_cfs_rq()`, `enqueue_task_fair()`, and `dequeue_entities()` also needed to account for the delayed count, since these functions walk the entity hierarchy adjusting `h_nr_running` and now also `h_nr_delayed`.

## Consequence

The observable impact is incorrect PELT load tracking signals, which has several cascading effects:

1. **Inflated `runnable_avg` on cfs_rq**: The `runnable_avg` signal is used by the scheduler to determine how loaded a CPU is. With delayed-dequeue tasks inflating this value, CPUs appear busier than they actually are. This directly biases load balancing decisions — the load balancer may choose to migrate tasks away from CPUs that are not actually overloaded, or may fail to pull tasks to CPUs that appear loaded but are in fact mostly idle.

2. **Incorrect cgroup share calculations**: For task groups (`CONFIG_FAIR_GROUP_SCHED`), the `runnable_weight` of group entities drives the weight/share distribution among competing groups. With inflated `runnable_weight`, a group with many sleeping (delayed-dequeue) tasks would receive a disproportionately large share of CPU time, starving other groups. The `load_avg` of cfs_rqs is kept artificially high, which biases `load_balance()` and cgroup shares as noted by Vincent Guittot in the discussion thread.

3. **Excessive CPU frequency on heterogeneous platforms**: On systems with energy-aware scheduling (EAS) and frequency scaling (schedutil), the inflated PELT signals can cause CPU frequencies to be set higher than necessary. As reported by Luis Machado and Hongyan Xia in the mailing list discussion, on ARM big.LITTLE platforms, little cores had their frequencies maxed out for extended periods (5-10+ seconds) despite being mostly idle, because delayed-dequeue tasks kept util_est and runnable values artificially high. This directly increased power consumption.

## Fix Summary

The fix introduces a new counter `cfs_rq->h_nr_delayed` that tracks the hierarchical count of delayed-dequeue entities within each CFS runqueue. Two new helper functions, `set_delayed()` and `clear_delayed()`, replace direct manipulation of `se->sched_delayed`. These functions walk the entity hierarchy (via `for_each_sched_entity`) to increment/decrement `h_nr_delayed` on each ancestor `cfs_rq`, stopping at throttled cfs_rqs (since throttled groups are already excluded from the hierarchy).

The key correction is in `__update_load_avg_cfs_rq()` (in `pelt.c`), where the runnable parameter now subtracts the delayed count:

```c
cfs_rq->h_nr_running - cfs_rq->h_nr_delayed,
```

This ensures only truly runnable tasks contribute to `cfs_rq->avg.runnable_avg`.

Similarly, `se_update_runnable()` in `sched.h` now computes group entity runnable weight as:

```c
se->runnable_weight = cfs_rq->h_nr_running - cfs_rq->h_nr_delayed;
```

The fix also propagates `h_nr_delayed` through all the paths that already propagate `h_nr_running`: `enqueue_task_fair()`, `dequeue_entities()`, `throttle_cfs_rq()`, and `unthrottle_cfs_rq()`. In `enqueue_task_fair()`, when a new task is being enqueued (via `task_new`, not a wakeup) and it has `sched_delayed` set, `h_nr_delayed` is incremented in each ancestor. In `dequeue_entities()`, when a non-sleeping, non-delayed task with `sched_delayed` is being dequeued, `h_nr_delayed` is decremented. The throttle/unthrottle paths delta `h_nr_delayed` similarly to how they already delta `h_nr_running` and `idle_h_nr_running`.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

- **Kernel version**: Linux v6.12-rc1 through v6.13-rc2 (the window between `fc1892becd56` being merged and this fix).
- **DELAY_DEQUEUE feature enabled**: The `DELAY_DEQUEUE` sched feature must be active (it is enabled by default in affected kernels).
- **CFS tasks that sleep and wake**: The bug manifests when tasks alternate between running and sleeping. When a task sleeps and is not eligible (has negative lag), it enters delayed-dequeue state. While in this state, it inflates `h_nr_running` without being truly runnable.
- **CONFIG_SMP enabled**: The PELT runnable tracking (`runnable_avg`) is only meaningful on SMP systems. On UP systems, the signal exists but has no load-balancing effect.
- **CONFIG_FAIR_GROUP_SCHED** (for the `se_update_runnable` path): The group entity runnable_weight inflation requires task groups/cgroups to be in use.
- **Multiple CPUs**: At least 2 CPUs are needed for the load balancing impact to be visible.
- **Workload characteristics**: The effect is strongest with many tasks that frequently sleep/wake on the same CPU. Each sleeping task that enters delayed-dequeue state adds 1 to `h_nr_running` without being runnable. If N tasks are delayed-dequeue on a CPU, the `runnable_avg` signal will behave as if N extra tasks are running, significantly inflating the signal.

The bug is deterministic in the sense that any CFS task that sleeps and enters delayed-dequeue state will trigger the accounting discrepancy. The magnitude of the observable impact depends on the workload intensity and the number of delayed-dequeue tasks at any given time.

## Reproduce Strategy (kSTEP)

The strategy is to create multiple CFS tasks that alternate between running and sleeping, causing them to accumulate in delayed-dequeue state, then observe that `cfs_rq->avg.runnable_avg` on the cfs_rq is inflated relative to the actual number of truly runnable tasks.

### Step-by-step plan:

1. **Topology setup**: Configure QEMU with at least 2 CPUs (e.g., `kstep_topo_init()` with default 2-CPU SMP). CPU 0 is reserved for the driver; use CPU 1 for the test tasks.

2. **Task creation**: Create 4-8 CFS tasks. Pin all tasks to CPU 1 using `kstep_task_pin(p, 1, 2)`. This concentrates all tasks on one CPU, maximizing the impact of delayed-dequeue on that CPU's `h_nr_running`.

3. **Generate delayed-dequeue state**: The key is to make tasks sleep while they have negative lag (i.e., they are not eligible). The DELAY_DEQUEUE feature will keep them on the runqueue with `sched_delayed = 1`.
   - Let all tasks run for a while using `kstep_tick_repeat(n)` to build up vruntime.
   - Then block tasks one at a time using `kstep_task_block(p)`. When a task is blocked via `dequeue_task_fair()` → `dequeue_entity()`, if `DELAY_DEQUEUE` is enabled and the entity is not eligible (`!entity_eligible(cfs_rq, se)`), the entity will get `sched_delayed = 1` and remain on the runqueue.

4. **Observe the discrepancy**: After blocking several tasks:
   - Use `KSYM_IMPORT` to access `cpu_rq(1)->cfs` (the root cfs_rq on CPU 1).
   - Read `cfs_rq->h_nr_running`: this will include delayed-dequeue entities.
   - Read `cfs_rq->h_nr_delayed` (on the fixed kernel, this field exists; on buggy kernel, it doesn't exist yet).
   - Read `cfs_rq->avg.runnable_avg`: on the buggy kernel, this will be inflated because the PELT calculation uses `h_nr_running` which includes delayed entities.
   - Count the actual number of truly runnable tasks (those with `se->sched_delayed == 0` and `se->on_rq == 1`).

5. **Detection logic (pass/fail criteria)**:
   - After blocking tasks that have negative lag, check `h_nr_running` on the cfs_rq.
   - On the **buggy kernel**: `cfs_rq->h_nr_running` will be greater than the number of actually runnable tasks. The `runnable_avg` will be driven upward by the inflated count. Specifically, if we have 1 truly runnable task and 3 delayed-dequeue tasks, `h_nr_running = 4` and `runnable_avg` will approach the value appropriate for 4 runnable tasks.
   - On the **fixed kernel**: `h_nr_running - h_nr_delayed` will equal the actual runnable count. The `runnable_avg` in `__update_load_avg_cfs_rq()` uses `h_nr_running - h_nr_delayed`, so it correctly reflects only the truly runnable task count.

6. **Alternative detection (works on both kernels)**: Since `h_nr_delayed` doesn't exist on the buggy kernel, a more robust approach:
   - Walk the entities on the cfs_rq and count those with `sched_delayed == 1`.
   - Compute `actual_runnable = h_nr_running - delayed_count`.
   - After several ticks (to let PELT decay/accrue), read `cfs_rq->avg.runnable_avg`.
   - Compare the `runnable_avg` to what would be expected for `actual_runnable` tasks vs `h_nr_running` tasks.
   - On the buggy kernel, `runnable_avg` will be significantly higher than expected for the actual runnable count.
   - On the fixed kernel, `runnable_avg` will match the actual runnable count.

7. **Callbacks**: Use `on_tick_end` callback to sample the PELT `runnable_avg` after each tick. After the blocking phase, continue ticking for several hundred ticks to let PELT converge, then compare the final `runnable_avg` value.

8. **Concrete sequence**:
   ```
   // Setup
   kstep_topo_init();  // 2+ CPUs
   task1 = kstep_task_create(); kstep_task_pin(task1, 1, 2);
   task2 = kstep_task_create(); kstep_task_pin(task2, 1, 2);
   task3 = kstep_task_create(); kstep_task_pin(task3, 1, 2);
   task4 = kstep_task_create(); kstep_task_pin(task4, 1, 2);
   
   // Let tasks run to accumulate vruntime
   kstep_tick_repeat(100);
   
   // Block 3 of 4 tasks — they should enter delayed-dequeue
   kstep_task_block(task2);
   kstep_task_block(task3);
   kstep_task_block(task4);
   
   // Let PELT converge with only 1 truly runnable task
   kstep_tick_repeat(500);
   
   // Read cfs_rq stats
   struct rq *rq = cpu_rq(1);
   struct cfs_rq *cfs_rq = &rq->cfs;
   
   // Count delayed entities by walking rb tree
   int delayed_count = 0;
   struct sched_entity *se;
   // ... iterate and count se->sched_delayed
   
   int actual_runnable = cfs_rq->h_nr_running - delayed_count;
   unsigned long runnable_avg = cfs_rq->avg.runnable_avg;
   
   // On buggy kernel: runnable_avg >> expected for 1 task
   // On fixed kernel: runnable_avg ~ expected for 1 task
   // The runnable_avg for 1 task on a CPU should approach ~1024
   // For 4 tasks it would approach ~4096
   if (runnable_avg > 2 * 1024 && actual_runnable <= 1)
       kstep_fail("runnable_avg inflated: %lu with only %d runnable",
                  runnable_avg, actual_runnable);
   else
       kstep_pass("runnable_avg correct: %lu with %d runnable",
                  runnable_avg, actual_runnable);
   ```

9. **Expected results**:
   - **Buggy kernel (v6.12-rc1 to v6.13-rc2)**: `runnable_avg` remains high (~4096 range) because PELT sees `h_nr_running = 4` (including 3 delayed entities). The driver should call `kstep_fail()`.
   - **Fixed kernel (v6.13-rc3+)**: `runnable_avg` converges toward ~1024 range because PELT now uses `h_nr_running - h_nr_delayed = 1`. The driver should call `kstep_pass()`.

10. **kSTEP changes needed**: None. The existing kSTEP APIs (`kstep_task_create`, `kstep_task_pin`, `kstep_task_block`, `kstep_tick_repeat`, `KSYM_IMPORT`, `cpu_rq()`, direct access to `cfs_rq->avg.runnable_avg` via `internal.h`) are sufficient to implement this driver. The cfs_rq fields `h_nr_running`, `avg.runnable_avg`, and the per-entity `sched_delayed` field are all accessible through kSTEP's internal scheduler headers.
