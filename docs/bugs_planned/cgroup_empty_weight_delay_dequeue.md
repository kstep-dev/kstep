# Cgroup: Empty Group Weight Recomputation Breaks DELAY_DEQUEUE

**Commit:** `66951e4860d3c688bfa550ea4a19635b57e00eca`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.13
**Buggy since:** v6.12-rc1 (introduced by commit `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

When the DELAY_DEQUEUE feature was introduced in the "Complete EEVDF" patch series, it changed how the CFS scheduler handles task dequeues. Instead of immediately removing a scheduling entity from the runqueue when it goes to sleep, DELAY_DEQUEUE retains the entity on the runqueue so it can continue to compete and "burn off" its accumulated lag. This is a key part of the EEVDF fairness model — it ensures that tasks which have received less than their fair share (positive lag) continue to be accounted for, preventing other tasks from unfairly consuming the leftover service time.

However, this new behavior creates a problematic interaction with fair group scheduling (CONFIG_FAIR_GROUP_SCHED). In cgroup-based scheduling, each cgroup has a group scheduling entity (`se`) on the parent cfs_rq, representing the aggregate weight of all tasks within that cgroup's per-CPU runqueue (`gcfs_rq`). The function `update_cfs_group()` periodically recomputes this group entity's weight based on the current state of the group's runqueue via `calc_group_shares()`.

When DELAY_DEQUEUE causes a group entity to remain enqueued despite its child runqueue (`gcfs_rq`) becoming empty (i.e., `gcfs_rq->load.weight == 0`), `calc_group_shares()` computes a minimal weight for the group entity — specifically, the floor value `MIN_SHARES` (which equals 2). This is correct for the non-DELAY_DEQUEUE case where an empty group would normally be dequeued, but it is catastrophically wrong when the entity is deliberately kept around to burn off lag at its original competitive weight.

The consequence is that the delayed entity's weight plummets from its original value (e.g., ~64 on a 16-CPU system with an active cgroup) to just 2. Since `reweight_entity()` preserves the entity's lag by scaling `vlag = vlag * old_weight / new_weight`, the lag value is inflated by a factor of ~32x (old_weight/new_weight). This inflated lag, combined with the entity's now-tiny weight in `avg_vruntime()` calculations (which uses `scale_load_down()`), creates severe distortions in the virtual runtime average, leading to wildly incorrect task placement and scheduling decisions.

## Root Cause

The root cause lies in the `update_cfs_group()` function in `kernel/sched/fair.c`. Before the fix, this function was:

```c
static void update_cfs_group(struct sched_entity *se)
{
    struct cfs_rq *gcfs_rq = group_cfs_rq(se);
    long shares;

    if (!gcfs_rq)
        return;

    if (throttled_hierarchy(gcfs_rq))
        return;

    shares = calc_group_shares(gcfs_rq);
    if (unlikely(se->load.weight != shares))
        reweight_entity(cfs_rq_of(se), se, shares);
}
```

The function only checked whether the entity was a group entity (has a `gcfs_rq`) and whether it was throttled. It did not check whether the group runqueue was empty. When `gcfs_rq->load.weight` is 0 (empty group), `calc_group_shares()` computes:

```c
load = max(scale_load_down(cfs_rq->load.weight), cfs_rq->avg.load_avg);
```

With `cfs_rq->load.weight == 0`, `load` falls to just `cfs_rq->avg.load_avg`, which is a PELT-decayed value that quickly approaches zero. The final shares computation then bottoms out at `MIN_SHARES` (value 2) due to the clamp:

```c
return clamp_t(long, shares, MIN_SHARES, tg_shares);
```

This triggers `reweight_entity()`, which performs several critical operations on the now-empty-but-still-enqueued group entity. First, it preserves the entity's virtual lag by scaling: `se->vlag = div_s64(se->vlag * se->load.weight, weight)`, where `weight` is the new very small value. Since old_weight >> new_weight, the vlag is inflated enormously. Second, it updates the entity's actual load weight to the tiny new value.

The inflated lag interacts catastrophically with `avg_vruntime()`. This function computes the weighted average vruntime of all entities on the cfs_rq:

```c
u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
    ...
    s64 avg = cfs_rq->avg_vruntime;
    long load = cfs_rq->avg_load;
    ...
    if (load) {
        if (avg < 0)
            avg -= (load - 1);
        avg = div_s64(avg, load);
    }
    return cfs_rq->min_vruntime + avg;
}
```

The `avg_vruntime` accumulator uses `scale_load_down(se->load.weight)` for each entity's contribution. While the delayed entity's weight contribution in `avg_load` is now tiny (scale_load_down(2) on 64-bit = 0 or 1), its `avg_vruntime` contribution — `key * weight` where `key = se->vruntime - min_vruntime` — can still be significant if the entity's vruntime has been pushed far from `min_vruntime` by the lag inflation. The combination of the ginormous lag (now encoded in the entity's vruntime via `place_entity()`) and the near-zero denominator weight creates a situation where `avg_vruntime()` can return wildly incorrect values.

On a 16-CPU machine with a normal active cgroup, the per-CPU group entity weight is approximately `tg_shares / nr_cpus`. For a default weight of 1024, that's about 64 per CPU. When reweighted to MIN_SHARES=2, the weight drops by a factor of 32. The lag gets scaled up by this same factor of 32 through the `se->vlag = div_s64(se->vlag * se->load.weight, weight)` calculation in `reweight_entity()`. This pushes the entity's vruntime far from the queue's average, and since `avg_vruntime()` is used by `place_entity()` to determine where new tasks are placed, all subsequent task placements on that CPU are corrupted.

## Consequence

The observable impact of this bug is severe scheduling latency spikes, particularly noticeable during task migrations and CPU affinity changes. Users reported `sched_setaffinity()` calls taking multiple seconds (up to 6 seconds observed) instead of the expected sub-millisecond duration. This manifests in several user-visible ways:

1. **Turbostat anomalies**: Doug Smythies reported that turbostat showed impossible TSC_MHz and PkgWatt readings (e.g., TSC_MHz of 879 instead of 4104, or 26667 with PkgWatt of 557W). These were caused by the timer sampling intervals being wildly distorted — a 1-second interval was actually taking 4.7 seconds in some cases because tasks were not being scheduled for extended periods.

2. **DL server crashes on ARM64**: Marcel Ziswiler reported kernel WARNINGs and crashes in `enqueue_dl_entity()` on Radxa ROCK 5B (arm64) boards, with "DL replenish lagged too much" messages. While this specific crash may have multiple contributing factors, the corrupted `avg_vruntime` values from the cgroup weight bug are part of the chain of failures.

3. **General scheduling stalls**: The corrupted `avg_vruntime()` causes `place_entity()` to assign wildly incorrect vruntimes to newly waking tasks. This can cause some tasks to receive far more CPU time than they deserve while others are starved. The effect is intermittent — occurring about 5% of the time according to user reports — but when it hits, the scheduling distortion can last for multiple seconds. The bug is most visible at near-100% CPU utilization where every scheduling decision matters and there is no idle slack to absorb the errors.

## Fix Summary

The fix adds a single additional check in `update_cfs_group()`: when the group's runqueue is empty (`gcfs_rq->load.weight == 0`), the function returns immediately without recomputing or updating the group entity's weight:

```c
static void update_cfs_group(struct sched_entity *se)
{
    struct cfs_rq *gcfs_rq = group_cfs_rq(se);
    long shares;

    /*
     * When a group becomes empty, preserve its weight. This matters for
     * DELAY_DEQUEUE.
     */
    if (!gcfs_rq || !gcfs_rq->load.weight)
        return;
    ...
}
```

This fix is correct because it preserves the DELAY_DEQUEUE invariant: a delayed entity should continue to compete at its original weight so it burns off its accumulated lag at the expected rate. When a group entity is kept around by DELAY_DEQUEUE, it should behave as though it still represents its tasks at the weight it had when those tasks were last active. Adjusting the weight of an empty-but-delayed group entity violates this invariant and introduces the cascading numerical errors described above.

The fix is also safe for the non-DELAY_DEQUEUE case: without DELAY_DEQUEUE, an empty group entity is immediately dequeued from its parent, so `update_cfs_group()` would only be called while the entity is on its way out. Even if the weight isn't updated in that brief window, it has no lasting effect because the entity is about to be removed from the tree entirely.

## Triggering Conditions

The following conditions are required to trigger this bug:

- **CONFIG_FAIR_GROUP_SCHED must be enabled**: The bug is in `update_cfs_group()` which only exists under `CONFIG_FAIR_GROUP_SCHED`. This is enabled by default in all major distribution kernels and whenever CONFIG_CGROUP_SCHED is active.

- **Multiple cgroups with CFS tasks**: At least one non-root cgroup must have CFS tasks. The system's default cgroup hierarchy (e.g., systemd's service slices) provides this automatically.

- **DELAY_DEQUEUE feature must be active**: This is enabled by default in kernels v6.12-rc1 and later (via `SCHED_FEAT(DELAY_DEQUEUE, true)`).

- **A per-CPU cgroup runqueue must become temporarily empty**: This happens naturally when a cgroup's tasks on a particular CPU all go to sleep or migrate away. On a multi-CPU system, this is extremely common — any cgroup that doesn't have tasks pinned to every CPU will frequently have empty per-CPU runqueues.

- **The group entity must be delay-dequeued (have positive lag)**: After all tasks leave a CPU's group cfs_rq, the group entity's dequeue is delayed if it has accumulated positive lag (received less than its fair share). The entity stays on the parent cfs_rq to burn off this lag.

- **`update_cfs_group()` must be called while the entity is delayed**: This function is called from `entity_tick()`, `enqueue_entity()`, and `dequeue_entity()` on every tick and every enqueue/dequeue in the same cfs_rq. Any activity on the parent cfs_rq (another entity being enqueued or ticked) will trigger this call.

- **Multiple CPUs**: The severity scales with CPU count. On a 16-CPU system, the per-CPU weight is ~1/16 of the total cgroup weight, so MIN_SHARES=2 represents a ~32x reduction from the normal ~64 weight, causing a ~32x lag inflation. On systems with more CPUs, the distortion is worse.

- **Near-100% CPU utilization**: User reports indicate the bug is most readily observable when the workload approaches 100% CPU usage. At lower utilization, idle time absorbs the scheduling errors, making them less visible.

The bug is probabilistic in manifestation but deterministic in mechanism: every time a group entity is delay-dequeued and then has its weight recomputed to MIN_SHARES, the lag inflation occurs. The visible impact depends on whether subsequent scheduling decisions happen to be affected by the corrupted `avg_vruntime()` value.

## Reproduce Strategy (kSTEP)

The reproduction strategy involves setting up a cgroup hierarchy with tasks that create the empty-group-then-delayed-dequeue scenario, and then observing the weight corruption and its effect on `avg_vruntime()`.

### Step-by-step Plan

1. **Configure QEMU with at least 4 CPUs** (more CPUs amplify the effect since per-CPU weight decreases while MIN_SHARES stays constant). Set `kstep_sysctl_write("kernel/sched_base_slice_ns", "%u", 3000000)` to use the default 3ms slice.

2. **Create a cgroup with a moderate weight**:
   ```c
   kstep_cgroup_create("testgrp");
   kstep_cgroup_set_weight("testgrp", 100);
   ```

3. **Create CFS tasks and add them to the cgroup**:
   - Create 2 CFS tasks (T1, T2).
   - Pin both tasks to CPU 1 using `kstep_task_pin(p, 1, 1)`.
   - Add both tasks to the "testgrp" cgroup via `kstep_cgroup_add_task("testgrp", task_pid_nr(p))`.
   - Wake both tasks with `kstep_task_wakeup(p)`.

4. **Run ticks to establish the group entity on CPU 1's parent cfs_rq**: Use `kstep_tick_repeat(20)` to let the tasks accumulate some runtime and establish stable PELT load averages. The group entity on CPU 1's parent cfs_rq will have a weight computed by `calc_group_shares()`.

5. **Record the initial state**: Using `KSYM_IMPORT()` and `cpu_rq()`, access the cfs_rq for CPU 1 and its parent cfs_rq. Record:
   - The group scheduling entity's `se->load.weight` (should be > MIN_SHARES=2, proportional to cgroup weight / nr_cpus).
   - The group scheduling entity's `se->vlag`.
   - `avg_vruntime()` of the parent cfs_rq.
   - The group cfs_rq's `load.weight` (should be non-zero with two tasks).

6. **Trigger the empty group condition on CPU 1**: Block or pause both tasks:
   ```c
   kstep_task_block(t1);
   kstep_task_block(t2);
   ```
   This causes `gcfs_rq->load.weight` to drop to 0. If DELAY_DEQUEUE is working, the group entity should remain on the parent cfs_rq with lag to burn off.

7. **Trigger `update_cfs_group()`**: Ensure there is at least one other entity on the parent cfs_rq of CPU 1 (either another cgroup's group entity or a root-level task). Create a task pinned to CPU 1 in the root cgroup and tick it. Each tick calls `entity_tick()` which calls `update_cfs_group()` for each entity. Alternatively, waking any task on CPU 1 will call `enqueue_entity()` → `update_cfs_group()`.
   ```c
   struct task_struct *trigger = kstep_task_create();
   kstep_task_pin(trigger, 1, 1);
   kstep_task_wakeup(trigger);
   kstep_tick_repeat(5);
   ```

8. **Observe the weight corruption**: After ticking, read the group entity's `se->load.weight` again. On the buggy kernel:
   - `se->load.weight` should have dropped to `MIN_SHARES * 1024` (i.e., `scale_load(2) = 2048` on 64-bit).
   - `se->vlag` should be inflated relative to the initial value by the ratio `old_weight / new_weight`.
   
   On the fixed kernel:
   - `se->load.weight` should remain at its original value (unchanged from step 5).
   - `se->vlag` should not be inflated.

9. **Detect the bug (pass/fail criteria)**: Access the group entity for the "testgrp" cgroup on CPU 1 using `cpu_rq(1)->cfs` to get the root cfs_rq, then iterate entities or use the `on_sched_group_alloc` callback to capture the task_group's `se[cpu]` pointer. Compare:
   ```c
   struct task_group *tg = /* obtained via cgroup or on_sched_group_alloc */;
   struct sched_entity *grp_se = tg->se[1]; // group entity on CPU 1
   struct cfs_rq *gcfs_rq = tg->cfs_rq[1]; // group's per-CPU cfs_rq
   
   if (gcfs_rq->load.weight == 0 && grp_se->on_rq) {
       // Group is empty but entity is delayed (DELAY_DEQUEUE active)
       long weight = grp_se->load.weight;
       s64 vlag = grp_se->vlag;
       
       if (weight <= scale_load(MIN_SHARES)) {
           kstep_fail("weight corrupted to %ld (MIN_SHARES), vlag=%lld", weight, vlag);
       } else {
           kstep_pass("weight preserved at %ld despite empty group", weight);
       }
   }
   ```

10. **kSTEP framework considerations**:
    - Use `KSYM_IMPORT(update_cfs_group)` if needed for tracing, though the key observation is reading `se->load.weight`.
    - Use `on_sched_group_alloc` callback to capture the `task_group` pointer when the cgroup is created.
    - The driver needs access to `struct task_group` internals. Since kSTEP includes `kernel/sched/sched.h` (via `internal.h`), `task_group->se[cpu]` and `task_group->cfs_rq[cpu]` are directly accessible.
    - The `on_tick_end` callback could be used to check the group entity state after each tick to catch the exact moment the weight changes.

### Expected Behavior

**Buggy kernel (v6.12-rc1 through v6.13-rc6)**: After the group's tasks are blocked and ticks advance, the group entity's weight drops to `scale_load(MIN_SHARES)` and its vlag is inflated by the ratio of old_weight to new_weight. The `avg_vruntime()` of the parent cfs_rq is distorted.

**Fixed kernel (v6.13+)**: After the group's tasks are blocked, the group entity remains on the parent cfs_rq (due to DELAY_DEQUEUE) with its original weight and vlag preserved. The `avg_vruntime()` remains accurate.

### Potential Complications

- The exact timing of when `update_cfs_group()` is called after the group empties matters. It happens on the next tick or enqueue/dequeue in the parent cfs_rq. Having another active task on the same CPU ensures ticks trigger `entity_tick()` → `update_cfs_group()` on the group entity.
- DELAY_DEQUEUE requires the entity to have positive lag when dequeued. If the tasks ran long enough to have negative lag (received more than fair share), the dequeue won't be delayed. Use short run durations or create asymmetric load to ensure positive lag.
- The `on_sched_group_alloc` callback is the cleanest way to get the `task_group` pointer. Alternatively, walk the cgroup hierarchy or use `css_for_each_descendant_pre()` from within the kernel module.
