# PELT: attach_entity_load_avg() Corner Case Truncates load_sum to Zero

**Commit:** `40f5aa4c5eaebfeaca4566217cb9c468e28ed682`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.18-rc4
**Buggy since:** v4.15-rc1 (introduced by `f207934fb79d` "sched/fair: Align PELT windows between cfs_rq and its se")

## Bug Description

The Per-Entity Load Tracking (PELT) subsystem maintains `load_avg` and `load_sum` fields for each scheduling entity (`sched_entity`) and each CFS runqueue (`cfs_rq`). A fundamental invariant of PELT is that `_avg` values must be zero whenever the corresponding `_sum` values are zero, because `_avg = _sum / divider`. The function `cfs_rq_is_decayed()` enforces this with a `SCHED_WARN_ON` assertion: if all `_sum` fields are zero but any `_avg` field is non-zero, a warning fires.

The function `attach_entity_load_avg()` is called when a scheduling entity is (re-)attached to a CFS runqueue. This happens during CPU migration (via `migrate_task_rq_fair()` → `enqueue_entity()` → `update_load_avg()` → `attach_entity_load_avg()`), during cgroup migration (via `sched_move_task()` → `task_change_group_fair()` → `attach_entity_cfs_rq()` → `attach_entity_load_avg()`), and during initial task creation (via `post_init_entity_util_avg()` → `attach_entity_cfs_rq()` → `attach_entity_load_avg()`).

In the buggy code, `attach_entity_load_avg()` recomputes `se->avg.load_sum` from the existing `se->avg.load_avg` using the formula `load_sum = (load_avg * divider) / se_weight(se)`. When the scheduling entity has a high weight (e.g., nice -20 corresponds to weight 88761 in `sched_prio_to_weight[]`) and a small `load_avg` value (e.g., 1), the integer division truncates the result to zero. This zero `load_sum` is then propagated to the `cfs_rq` totals via `enqueue_load_avg()`, which adds `se_weight(se) * se->avg.load_sum = 88761 * 0 = 0` to `cfs_rq->avg.load_sum` but adds `se->avg.load_avg = 1` to `cfs_rq->avg.load_avg`. This breaks the `_avg`/`_sum` consistency invariant.

The bug was discovered and reported by Kuyo Chang at MediaTek, who observed the `SCHED_WARN_ON` in `cfs_rq_is_decayed()` firing on production devices running workloads involving high-priority (low nice value) tasks that experienced PELT load decay to very small values before being reattached to a CFS runqueue.

## Root Cause

The root cause is an integer division truncation in the `attach_entity_load_avg()` function in `kernel/sched/fair.c`. The buggy code is:

```c
se->avg.load_sum = divider;
if (se_weight(se)) {
    se->avg.load_sum =
        div_u64(se->avg.load_avg * se->avg.load_sum, se_weight(se));
}
```

This first sets `load_sum = divider` (which is `get_pelt_divider(&cfs_rq->avg)`, typically in the range 46718–47742), then computes `load_sum = (load_avg * divider) / se_weight`. The intent is to reverse the relationship `load_avg = se_weight * load_sum / divider` to recover `load_sum` from `load_avg`.

The problem occurs when `se_weight(se) > load_avg * divider`. In this case, the unsigned 64-bit division `div_u64(load_avg * divider, se_weight)` truncates to zero. The PELT divider (`get_pelt_divider()`) is computed as `PELT_MIN_DIVIDER + avg->period_contrib`, where `PELT_MIN_DIVIDER = LOAD_AVG_MAX - 1024 = 47742 - 1024 = 46718` and `period_contrib` ranges from 0 to 1023. Therefore the maximum divider value is 47741.

For a task with nice value -20, `se_weight(se) = scale_load(88761) = 88761` (on 32-bit architectures without `CONFIG_64BIT`; with 64-bit scaling the weight is even larger). Since `88761 > 47741`, when `load_avg = 1`, the computation becomes `div_u64(1 * 47741, 88761) = 0`. The same issue affects nice -19 (weight 71755) and nice -18 (weight 56483), as all have weights exceeding the maximum possible divider value.

After this zero `load_sum` is computed, `enqueue_load_avg()` executes:

```c
cfs_rq->avg.load_avg += se->avg.load_avg;         // += 1
cfs_rq->avg.load_sum += se_weight(se) * se->avg.load_sum;  // += 88761 * 0 = 0
```

The `cfs_rq` now has `load_avg = 1` but `load_sum = 0`. When this entity is later dequeued and the CFS runqueue fully decays, `cfs_rq_is_decayed()` checks:

```c
if (cfs_rq->avg.load_sum)
    return false;
// ...
SCHED_WARN_ON(cfs_rq->avg.load_avg ||
              cfs_rq->avg.util_avg ||
              cfs_rq->avg.runnable_avg);
```

Since `load_sum` is 0 but `load_avg` is still 1 (it was never properly subtracted because the `load_sum` inconsistency prevented normal decay), the `SCHED_WARN_ON` fires.

The deeper issue is that the original formula attempts to divide by `se_weight` to convert from the weighted `load_avg` domain back to the unweighted `load_sum` domain, but this conversion is lossy due to integer arithmetic when the weight is large relative to `load_avg * divider`.

## Consequence

The immediate consequence is a `SCHED_WARN_ON` firing in `cfs_rq_is_decayed()`, producing a kernel warning with a stack trace. The warning callchain observed in the original bug report is:

```
Call trace:
  __update_blocked_fair
  update_blocked_averages
  newidle_balance
  pick_next_task_fair
  __schedule
  schedule
  pipe_read
  vfs_read
  ksys_read
```

This warning fires during `newidle_balance()`, which is called from `pick_next_task_fair()` when a CPU goes idle and tries to steal work from other CPUs. During this process, `update_blocked_averages()` iterates over all CFS runqueues with pending blocked load and checks if they are fully decayed via `cfs_rq_is_decayed()`.

Beyond the warning itself, the inconsistency between `load_avg` and `load_sum` can cause subtle PELT accounting errors. The `load_avg` value of 1 effectively becomes "sticky" — it cannot be properly removed through the normal PELT decay mechanism because `load_sum` is already 0 and cannot decay further. This orphaned `load_avg` contribution can affect:

1. **Load balancing decisions**: `cfs_rq->avg.load_avg` is used by the load balancer (`find_busiest_group()`, `find_busiest_queue()`) to determine how loaded a CPU is. A phantom load_avg of 1 is tiny but violates the PELT invariants, and accumulated over many occurrences could skew balancing decisions.
2. **Task group load propagation**: `update_tg_load_avg()` uses `cfs_rq->avg.load_avg` to compute task group load contributions. An incorrect value here propagates up the cgroup hierarchy.
3. **CFS runqueue lifecycle**: A CFS runqueue with non-zero `load_avg` but zero `load_sum` may not be properly removed from the `leaf_cfs_rq_list`, causing unnecessary iterations in `update_blocked_averages()`.

While the bug does not cause a kernel crash or data corruption, the persistent `SCHED_WARN_ON` messages can fill the kernel log, and the underlying PELT accounting inconsistency degrades the accuracy of the scheduler's load tracking infrastructure.

## Fix Summary

The fix replaces the old `load_sum` calculation with a new formula that guarantees `load_sum` is never zero when `load_avg` is non-zero. The new code is:

```c
se->avg.load_sum = se->avg.load_avg * divider;
if (se_weight(se) < se->avg.load_sum)
    se->avg.load_sum = div_u64(se->avg.load_sum, se_weight(se));
else
    se->avg.load_sum = 1;
```

The new formula first computes `load_avg * divider` without dividing by `se_weight`. It then checks whether `se_weight` is less than this product. If so, the division `load_sum / se_weight` will yield at least 1, so it proceeds with the division. If not (i.e., `se_weight >= load_avg * divider`), the division would truncate to 0, so the code clamps `load_sum` to 1 instead.

This is mathematically correct because the relationship `load_avg = se_weight * load_sum / divider` implies `load_sum = load_avg * divider / se_weight`. When `load_avg >= 1` and `se_weight >= load_avg * divider`, the true `load_sum` value is in the range (0, 1), so clamping to 1 is the closest valid integer approximation. The comment in the original code even acknowledges that "since we're entirely outside of the PELT hierarchy, nobody cares if we truncate _sum a little" — the fix simply ensures the truncation doesn't go all the way to zero.

The fix also implicitly handles the `se_weight(se) == 0` edge case: if weight is 0, the condition `se_weight(se) < se->avg.load_sum` would be true (since `load_sum = load_avg * divider >= 0`), but `div_u64(load_sum, 0)` would be a division by zero. However, `se_weight(se)` can only be 0 for a dequeued entity with no shares, and `load_avg` would also be 0 in that case, so `load_sum = 0 * divider = 0`, making the condition `0 < 0` false and taking the `else` branch (setting `load_sum = 1`, which is safe since `load_avg` is 0 and this is a no-op path).

## Triggering Conditions

The bug requires the following specific conditions:

1. **A task with a high scheduling weight**: The task must have a `se_weight` value greater than the PELT divider. In the `sched_prio_to_weight[]` table, this corresponds to nice values -20 (weight 88761), -19 (weight 71755), or -18 (weight 56483). The maximum PELT divider is approximately 47742, so any weight above ~47742 can trigger the bug. Nice -17 (weight 46273) is borderline and could trigger depending on the exact `period_contrib` value.

2. **Small `load_avg` value at attach time**: The scheduling entity must have `se->avg.load_avg = 1` (or more generally, `load_avg * divider < se_weight`) when `attach_entity_load_avg()` is called. This occurs when the task has been sleeping (blocked) for a prolonged period, causing its PELT load to decay. For a fully saturated nice -20 task (running continuously), `load_avg` would be 88761. To decay to `load_avg = 1`, approximately 526 milliseconds (ticks) of sleep are needed. For a task that ran only briefly (e.g., 5 ticks), the initial `load_avg` is much smaller (~9319) and decays to 1 after approximately 422 ticks of sleep.

3. **An attach event**: The task must be attached to a CFS runqueue via one of these code paths:
   - **CPU migration**: The task wakes up and is placed on a different CPU than where it last ran. `migrate_task_rq_fair()` sets `se->avg.last_update_time = 0`, and the subsequent `enqueue_entity()` calls `update_load_avg()` which invokes `attach_entity_load_avg()` when it detects `!se->avg.last_update_time && (flags & DO_ATTACH)`.
   - **Cgroup migration**: The task is moved to a different cgroup via `cgroup.procs` write, triggering `sched_move_task()` → `task_change_group_fair()` → `detach_entity_cfs_rq()` + `attach_entity_cfs_rq()`.
   - **Initial fork/exec**: `post_init_entity_util_avg()` calls `attach_entity_cfs_rq()` → `attach_entity_load_avg()` for newly created tasks (though new tasks typically have `load_avg` initialized to larger values).

4. **CONFIG_SMP enabled**: The `enqueue_load_avg()` and `dequeue_load_avg()` functions are only compiled (non-empty) under `CONFIG_SMP`. Without SMP, the functions are no-ops and the bug cannot manifest.

5. **CONFIG_FAIR_GROUP_SCHED** (for cgroup path): Cgroup migration only triggers `attach_entity_load_avg()` if `CONFIG_FAIR_GROUP_SCHED` is enabled.

The bug is deterministic once the conditions are met: any time a nice -20/-19/-18 task with `load_avg = 1` is reattached, `load_sum` will be set to 0. The `SCHED_WARN_ON` then fires the next time `cfs_rq_is_decayed()` is evaluated for that CFS runqueue after all tasks have been dequeued.

## Reproduce Strategy (kSTEP)

This bug can be reproduced using kSTEP by creating a high-priority task (nice -20), letting its PELT load decay to the critical value of 1, and then triggering a cgroup migration to invoke `attach_entity_load_avg()` with the problematic `load_avg`.

### Step-by-step plan:

1. **Configure QEMU with at least 2 CPUs**: The bug requires `CONFIG_SMP`, which needs multiple CPUs. Use the default kSTEP QEMU configuration or ensure at least 2 CPUs.

2. **Create cgroups**: Create two cgroups for the task migration:
   ```c
   kstep_cgroup_create("grp_a");
   kstep_cgroup_create("grp_b");
   ```

3. **Create a CFS task with nice -20**: This gives weight 88761, which exceeds the maximum PELT divider (~47742):
   ```c
   struct task_struct *task = kstep_task_create();
   kstep_task_pin(task, 1, 1);  // Pin to CPU 1 (avoid CPU 0)
   kstep_task_set_prio(task, -20);  // Nice -20 → weight 88761
   kstep_cgroup_add_task("grp_a", task->pid);
   ```

4. **Let the task run to accumulate PELT load**: Run the task for approximately 10 ticks so its `load_avg` builds up to a meaningful value:
   ```c
   kstep_tick_repeat(10);
   ```

5. **Block the task and let PELT decay**: Block the task and advance time to let the PELT `load_avg` decay toward 1. After approximately 400–500 ticks of being blocked, `load_avg` should reach the critical value:
   ```c
   kstep_task_block(task);
   ```
   Then tick repeatedly, checking `se->avg.load_avg` each time:
   ```c
   struct sched_entity *se = &task->se;
   for (int i = 0; i < 600; i++) {
       kstep_tick();
       if (se->avg.load_avg <= 1 && se->avg.load_avg > 0)
           break;
   }
   ```

6. **Record the pre-attach PELT state**: Before triggering the cgroup migration, log the scheduling entity's PELT fields:
   ```c
   kstep_pass("Pre-attach: load_avg=%lu load_sum=%llu se_weight=%lu divider=%u",
              se->avg.load_avg, se->avg.load_sum,
              se_weight(se), get_pelt_divider(&se->avg));
   ```

7. **Trigger cgroup migration to invoke attach_entity_load_avg**: Move the task to the second cgroup. This calls `sched_move_task()` → `task_change_group_fair()` → `detach_entity_cfs_rq()` → `attach_entity_cfs_rq()` → `attach_entity_load_avg()`:
   ```c
   kstep_cgroup_add_task("grp_b", task->pid);
   ```

8. **Check the CFS runqueue PELT state for inconsistency**: After the attach, read the CFS runqueue's PELT values. On the buggy kernel, `load_sum` will be 0 while `load_avg` is 1:
   ```c
   struct cfs_rq *cfs_rq = cfs_rq_of(se);
   unsigned long load_avg = cfs_rq->avg.load_avg;
   u64 load_sum = cfs_rq->avg.load_sum;
   kstep_pass("Post-attach cfs_rq: load_avg=%lu load_sum=%llu", load_avg, load_sum);

   // Also check the se itself
   kstep_pass("Post-attach se: load_avg=%lu load_sum=%llu",
              se->avg.load_avg, (unsigned long long)se->avg.load_sum);
   ```

9. **Determine pass/fail**: On the **buggy kernel** (pre-fix), after the cgroup move, `se->avg.load_sum` should be 0 while `se->avg.load_avg` is 1 (or a small non-zero value). This means `cfs_rq->avg.load_sum` includes `se_weight * 0 = 0` while `cfs_rq->avg.load_avg` includes 1. The detection logic:
   ```c
   if (se->avg.load_avg > 0 && se->avg.load_sum == 0) {
       kstep_fail("BUG: load_avg=%lu but load_sum=0 after attach",
                  se->avg.load_avg);
   } else if (se->avg.load_avg > 0 && se->avg.load_sum > 0) {
       kstep_pass("OK: load_avg=%lu load_sum=%llu consistent",
                  se->avg.load_avg, (unsigned long long)se->avg.load_sum);
   }
   ```

   On the **fixed kernel**, `se->avg.load_sum` will be at least 1 when `load_avg > 0`, so the consistency check passes.

### Callback considerations:

No special callbacks (`on_tick_begin`, `on_sched_softirq_end`, etc.) are needed for this reproduction. The driver operates in a straightforward sequential manner: create task, run, block, decay, migrate, check.

### KSYM_IMPORT requirements:

The driver needs access to PELT internals. The `se->avg` struct fields (`load_avg`, `load_sum`) are available through `kernel/sched/sched.h` which kSTEP's `internal.h` provides. The `se_weight()` function and `get_pelt_divider()` macro from `kernel/sched/pelt.h` may need to be imported or re-implemented locally:
```c
// se_weight is static inline in fair.c, may need KSYM_IMPORT or local helper
static inline unsigned long local_se_weight(struct sched_entity *se) {
    return scale_load_down(se->load.weight);
}
```

### Alternative approach (direct CPU migration):

Instead of cgroups, another approach is to trigger CPU migration:
1. Create the nice -20 task pinned to CPU 1.
2. Let it run and accumulate load.
3. Block and let load decay to `load_avg = 1`.
4. Change the task's CPU affinity to CPU 2 (`kstep_task_pin(task, 2, 2)`).
5. Wake the task, which triggers migration to CPU 2 and invokes `attach_entity_load_avg()` on CPU 2's CFS runqueue.
6. Check the PELT state on CPU 2's CFS runqueue.

This approach avoids the need for `CONFIG_FAIR_GROUP_SCHED` but requires verifying that the affinity change while blocked correctly triggers migration on wakeup.

### Expected results:

- **Buggy kernel (v4.15-rc1 to v5.18-rc3)**: After the attach, `se->avg.load_sum == 0` while `se->avg.load_avg == 1`. If the CFS runqueue subsequently decays, `cfs_rq_is_decayed()` triggers `SCHED_WARN_ON`. The kernel log will contain the warning with the call trace through `__update_blocked_fair`.
- **Fixed kernel (v5.18-rc4+)**: After the attach, `se->avg.load_sum >= 1` whenever `se->avg.load_avg > 0`. The clamping to 1 in the `else` branch prevents the zero truncation. No warning fires and PELT invariants are maintained.

### Robustness notes:

The exact number of ticks required for `load_avg` to decay to 1 depends on the initial load accumulation and the PELT half-life (32ms). The driver should use a loop with a condition check rather than a fixed tick count, monitoring `se->avg.load_avg` each tick until it reaches the target value of 1. If `load_avg` decays past 1 to 0 before the check, the driver should retry with different initial run durations (e.g., more ticks of initial running to build a higher peak load, giving more time at `load_avg = 1` during decay).
