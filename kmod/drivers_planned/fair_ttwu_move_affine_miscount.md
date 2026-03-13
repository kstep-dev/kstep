# Fair: Inaccurate Tally of ttwu_move_affine in wake_affine()

**Commit:** `39afe5d6fc59237ff7738bf3ede5a8856822d59d`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.4-rc1
**Buggy since:** v4.16-rc1 (introduced by commit `806486c377e33` "sched/fair: Do not migrate if the prev_cpu is idle")

## Bug Description

The `wake_affine()` function in the CFS scheduler decides whether to migrate a waking task to the current CPU (`this_cpu`) or keep it on the previous CPU (`prev_cpu`). It uses two heuristics: `wake_affine_idle()` (checks idle states) and `wake_affine_weight()` (compares load). After making the decision, `wake_affine()` updates schedstats counters to record whether the wakeup was "affine" (i.e., the task was moved to `this_cpu`).

The bug is a logic error in how `wake_affine()` determines whether the wakeup decision was affine or not. Prior to commit `806486c377e33`, `wake_affine_idle()` could only return `this_cpu` (affine) or `nr_cpumask_bits` (no decision). That commit added a third possible return value: `prev_cpu`, which means "keep the task on prev_cpu because it is idle and shares cache with this_cpu." However, the check in `wake_affine()` was not updated to account for this new return value.

The check `target == nr_cpumask_bits` was designed to distinguish between "affine wakeup" and "no affine decision." But when `wake_affine_idle()` returns `prev_cpu`, `target` becomes `prev_cpu`, which is neither `nr_cpumask_bits` (no decision) nor `this_cpu` (affine). The old check treats this as an affine wakeup and increments `sd->ttwu_move_affine` and `p->stats.nr_wakeups_affine`, even though the task is staying on `prev_cpu` — the exact opposite of an affine migration.

## Root Cause

The function `wake_affine_idle()` was modified by commit `806486c377e33` to handle the case where both `this_cpu` and `prev_cpu` are idle and share cache. The change was:

```c
if (idle_cpu(this_cpu) && cpus_share_cache(this_cpu, prev_cpu))
    return idle_cpu(prev_cpu) ? prev_cpu : this_cpu;
```

Before this change, the function returned only `this_cpu` or `nr_cpumask_bits`. After the change, it can also return `prev_cpu` — a valid CPU number that is neither `this_cpu` nor the sentinel value `nr_cpumask_bits`.

In `wake_affine()`, the logic after consulting the heuristics was:

```c
int target = nr_cpumask_bits;

if (sched_feat(WA_IDLE))
    target = wake_affine_idle(this_cpu, prev_cpu, sync);

if (sched_feat(WA_WEIGHT) && target == nr_cpumask_bits)
    target = wake_affine_weight(sd, p, this_cpu, prev_cpu, sync);

schedstat_inc(p->stats.nr_wakeups_affine_attempts);
if (target == nr_cpumask_bits)    /* <-- THE BUG */
    return prev_cpu;

schedstat_inc(sd->ttwu_move_affine);
schedstat_inc(p->stats.nr_wakeups_affine);
return target;
```

When `wake_affine_idle()` returns `prev_cpu` (because both CPUs are idle and share cache):

1. `target = prev_cpu` (a valid CPU number, not `nr_cpumask_bits`)
2. The `target == nr_cpumask_bits` check in the `WA_WEIGHT` guard evaluates to FALSE, so `wake_affine_weight()` is correctly skipped.
3. The `target == nr_cpumask_bits` check for the stats evaluates to FALSE, so the function does NOT take the early `return prev_cpu` path.
4. Instead, it falls through to `schedstat_inc(sd->ttwu_move_affine)` and `schedstat_inc(p->stats.nr_wakeups_affine)`, then returns `target` (which is `prev_cpu`).

The result is that the task is placed on `prev_cpu` (correct behavior — the task stays where it was), but the schedstats counters record this as an affine migration to `this_cpu` (incorrect accounting). The `ttwu_move_affine` counter is supposed to count only cases where a task is moved to the waking CPU, but here it counts a non-migration event.

Importantly, although the accounting is wrong, the actual task placement is correct in all cases: when `target == prev_cpu`, the function returns `prev_cpu` regardless of which code path is taken (the early return also returns `prev_cpu`). As Peter Zijlstra noted in the mailing list review: "This not only changes the accounting but also the placement, no?" — but in fact, the placement is unchanged because both paths return `prev_cpu` when `target == prev_cpu`.

## Consequence

The observable impact is that `schedstat` counters are inflated for affine wakeups. Specifically:

- `sd->ttwu_move_affine` (visible via `/proc/schedstat` or debugfs) is incremented for wakeups that kept the task on `prev_cpu` rather than migrating to `this_cpu`. This makes it appear that more affine migrations are happening than actually are.
- `p->stats.nr_wakeups_affine` (visible via `/proc/<pid>/sched`) is similarly inflated, making per-task affine wakeup statistics unreliable.

This does not cause crashes, hangs, or incorrect scheduling decisions. The task placement algorithm works correctly — the task ends up on the right CPU. However, any tooling or analysis that relies on schedstats for performance tuning, scheduler debugging, or workload characterization will see misleading data. For example, a developer analyzing migration patterns might conclude that the scheduler is aggressively pulling tasks to the waking CPU when in reality the tasks are staying put. This can lead to incorrect conclusions about scheduler behavior, misguided tuning decisions, or confusion when debugging performance issues.

The bug is triggered on every wakeup where both `this_cpu` and `prev_cpu` are idle and share cache (and `prev_cpu != this_cpu`). On systems with light loads where many CPUs are idle, this scenario is common, making the miscounting significant in production environments.

## Fix Summary

The fix replaces the sentinel-value check `target == nr_cpumask_bits` with the semantically correct check `target != this_cpu`:

```c
-	if (target == nr_cpumask_bits)
+	if (target != this_cpu)
		return prev_cpu;
```

This correctly classifies the three possible return values from the heuristics:

| `target` value     | Old check (`== nr_cpumask_bits`) | New check (`!= this_cpu`) | Correct? |
|---------------------|----------------------------------|---------------------------|----------|
| `this_cpu`          | FALSE → count as affine          | FALSE → count as affine   | ✓        |
| `prev_cpu`          | FALSE → count as affine          | TRUE → return prev_cpu    | ✓ (fixed)|
| `nr_cpumask_bits`   | TRUE → return prev_cpu           | TRUE → return prev_cpu    | ✓        |

The fix is both minimal and complete. It correctly handles all three possible return values by recognizing that an affine wakeup is one where `target == this_cpu`, and anything else (whether `prev_cpu` or the sentinel `nr_cpumask_bits`) means the task was not migrated to the waking CPU. The actual task placement remains unchanged because both the old and new code paths return `prev_cpu` when `target == prev_cpu`; only the statistics tracking is corrected.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger the miscounting:

1. **CONFIG_SCHEDSTATS=y**: The kernel must be built with schedstats enabled, otherwise the `schedstat_inc()` calls are no-ops and the bug is invisible. This is common in distribution kernels built for profiling/debugging.

2. **At least 2 CPUs sharing cache**: The `cpus_share_cache(this_cpu, prev_cpu)` check must return true. This is satisfied when both CPUs are in the same LLC (last-level cache) domain, which is the common case for CPUs in the same socket on modern hardware.

3. **WA_IDLE sched feature enabled**: `sched_feat(WA_IDLE)` must be true, which is the default.

4. **this_cpu is idle**: `idle_cpu(this_cpu)` must return true. The `this_cpu` is the CPU executing `try_to_wake_up()`.

5. **prev_cpu is idle**: `idle_cpu(prev_cpu)` must return true. The `prev_cpu` is the CPU on which the task last ran.

6. **this_cpu ≠ prev_cpu**: If they are the same CPU, `wake_affine_idle()` returns `this_cpu` (which equals `prev_cpu`), and the old check works correctly.

7. **The task is a CFS task**: `wake_affine()` is only called from `select_task_rq_fair()`, so the task must use `SCHED_NORMAL`, `SCHED_BATCH`, or `SCHED_IDLE` policy.

8. **The wakeup traverses wake_affine path**: The task must be woken up via `try_to_wake_up()` where `select_task_rq_fair()` considers the wake-affine heuristic. This happens when `want_affine` is true in `select_task_rq_fair()`, which requires the task to be allowed to run on `this_cpu` and the `SD_WAKE_AFFINE` flag to be set in the scheduling domain.

The scenario is common on lightly loaded multi-core systems: a task sleeps on CPU 1, an interrupt or another task on CPU 0 wakes it, and both CPUs are idle. The scheduler correctly decides to keep the task on CPU 1 (to preserve cache locality), but the buggy code incorrectly records this as an affine migration.

## Reproduce Strategy (kSTEP)

The goal is to demonstrate that on the buggy kernel, `ttwu_move_affine` is incremented when a task stays on `prev_cpu`, whereas on the fixed kernel, it is not.

### Step 1: Topology Setup

Configure QEMU with at least 2 CPUs. Set up a topology where CPU 0 and CPU 1 share a cache domain:

```c
kstep_topo_init();
kstep_topo_set_mc(0, 1);  // CPUs 0 and 1 share MC-level cache
kstep_topo_apply();
```

This ensures `cpus_share_cache(0, 1)` returns true.

### Step 2: Enable SCHEDSTATS

Ensure the kernel is built with `CONFIG_SCHEDSTATS=y`. If schedstats can be toggled at runtime, enable it via:

```c
kstep_sysctl_write("kernel/sched_schedstats", "%d", 1);
```

### Step 3: Create the Task

Create a CFS task and pin it to CPUs 0 and 1 (allowing both as possible CPUs):

```c
struct task_struct *p = kstep_task_create();
kstep_task_pin(p, 1, 2);  // Allow CPUs 1 and 2 (or use a cpumask)
```

Alternatively, create a kthread that can be scheduled on CPU 1:

```c
struct task_struct *t = kstep_kthread_create("test_task");
kstep_kthread_bind(t, cpumask_of(1));
kstep_kthread_start(t);
```

### Step 4: Establish prev_cpu

Run the task on CPU 1 to establish `prev_cpu = 1`. Let it run briefly then block:

```c
kstep_kthread_start(t);
kstep_tick_repeat(5);  // Let it run on CPU 1
kstep_kthread_block(t);  // Block the task; prev_cpu is now 1
kstep_tick_repeat(5);  // Let CPU 1 go idle
```

### Step 5: Read Schedstats Before Wakeup

Use `KSYM_IMPORT` and internal scheduler headers to access the schedstats counters. Import the relevant `sched_domain` for CPU 0 and read `ttwu_move_affine`:

```c
struct rq *rq0 = cpu_rq(0);
struct sched_domain *sd;
unsigned long affine_before;

rcu_read_lock();
sd = rcu_dereference(rq0->sd);  // Get the lowest sched domain for CPU 0
if (sd)
    affine_before = sd->ttwu_move_affine;
rcu_read_unlock();
```

Also read the per-task counter:

```c
unsigned long task_affine_before = p->stats.nr_wakeups_affine;
```

### Step 6: Trigger the Wakeup from CPU 0

Wake the task from CPU 0 (the driver CPU). Since the driver runs on CPU 0, a simple wakeup call will have `this_cpu = 0`:

```c
kstep_kthread_syncwake(current, t);  // Wake from CPU 0; prev_cpu=1, this_cpu=0
```

Or if using kstep tasks:

```c
kstep_task_wakeup(p);  // Wakeup from CPU 0
```

Both CPUs should be idle at this point (CPU 0 running the driver's idle-like context, CPU 1 with no runnable tasks).

### Step 7: Read Schedstats After Wakeup

```c
unsigned long affine_after;
rcu_read_lock();
sd = rcu_dereference(rq0->sd);
if (sd)
    affine_after = sd->ttwu_move_affine;
rcu_read_unlock();

unsigned long task_affine_after = p->stats.nr_wakeups_affine;
```

### Step 8: Verify the Bug

The task should have been placed on CPU 1 (prev_cpu) because both CPUs are idle and share cache. On the buggy kernel, the counters will have been incremented:

```c
if (affine_after > affine_before) {
    kstep_fail("ttwu_move_affine incremented (%lu -> %lu) but task stayed on prev_cpu=%d",
               affine_before, affine_after, task_cpu(p));
} else {
    kstep_pass("ttwu_move_affine correctly not incremented when task stayed on prev_cpu");
}
```

### Expected Results

- **Buggy kernel**: `ttwu_move_affine` is incremented (the counter increases by 1) even though the task remained on CPU 1. `kstep_fail()` is triggered.
- **Fixed kernel**: `ttwu_move_affine` is NOT incremented because the task was not migrated to `this_cpu`. `kstep_pass()` is triggered.

### Additional Considerations

- The driver should verify that the task actually ended up on CPU 1 (`task_cpu(p) == 1`) to confirm the placement did not change, only the accounting.
- Repeat the wakeup cycle multiple times to ensure the miscount accumulates predictably (e.g., N wakeups should produce exactly N extra `ttwu_move_affine` counts on the buggy kernel, and zero extra on the fixed kernel).
- If `CONFIG_SCHEDSTATS` is not available in the kernel build, the bug is unobservable and the test should be skipped with a diagnostic message.
- The `WA_IDLE` sched feature must be enabled (it is by default). If it is not, `wake_affine_idle()` is never called and the bug path is unreachable. This can be verified by checking `sched_feat(WA_IDLE)` or reading `/sys/kernel/debug/sched/features`.
- Ensure no other tasks are runnable on CPUs 0 or 1 at the time of wakeup, so that `idle_cpu()` returns true for both. The kstep driver should drain any pending work before the test.

### kSTEP Changes Needed

No fundamental changes to kSTEP are required. The driver needs:
1. Access to `struct sched_domain` via `rcu_dereference(cpu_rq(cpu)->sd)` — already available through `internal.h`.
2. Access to `p->stats.nr_wakeups_affine` — already available since `struct task_struct` is accessible.
3. `CONFIG_SCHEDSTATS=y` in the kernel config — this is a build-time requirement.
4. A topology with shared cache between CPUs 0 and 1 — achievable via `kstep_topo_*()` APIs.

The only potential minor addition would be a helper to read schedstats counters safely, but this can be done directly in the driver using existing internal access.
