# Bandwidth: Unthrottle Encounters Sched_delayed Group Entity

**Commit:** `9b5ce1a37e904fac32d560668134965f4e937f6c`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.12-rc4
**Buggy since:** `152e11f6df29` ("sched/fair: Implement delayed dequeue"), merged in v6.12-rc1

## Bug Description

When a CFS bandwidth-limited cgroup's `cfs_rq` is unthrottled (after new bandwidth becomes available), `unthrottle_cfs_rq()` walks up the scheduling entity hierarchy to re-enqueue group entities into their parent `cfs_rq`s. If it encounters an ancestor group entity that has `sched_delayed = true` (i.e., it was marked for delayed dequeue by the EEVDF mechanism), the old code did not know how to handle this state. It would trigger `SCHED_WARN_ON(se->sched_delayed)` and then `break` out of the loop prematurely, leaving the hierarchy in an inconsistent state.

The delayed dequeue feature, introduced in commit `152e11f6df29`, allows scheduling entities that go to sleep to remain on the run queue's rbtree with `sched_delayed = true` and `on_rq = true`. This postpones the actual removal until the entity is next "picked" by the scheduler. The feature applies to both task entities and group entities. However, `unthrottle_cfs_rq()` was not updated to handle the case where a group entity along the hierarchy walk has been treated with delayed dequeue.

The interaction occurs in nested cgroup hierarchies (at least 3 levels: root → A → B). When B's `cfs_rq` is throttled due to bandwidth exhaustion, B's group entity is removed from A's `cfs_rq`. If, while B is throttled, all other tasks under A also go to sleep, A's group entity in the root `cfs_rq` can enter the `sched_delayed` state. When B's bandwidth is later replenished and `unthrottle_cfs_rq()` fires, it re-enqueues B's entity into A's `cfs_rq` and then walks up to A's entity in the root `cfs_rq` — where it finds `on_rq = true` and `sched_delayed = true`. The old code was not prepared for this combination.

The bug was originally reported by Venkat Rao Bagalkote, who observed kernel crashes and warnings while running the LTP FS Stress test suite on a PowerPC POWER9 system with kernel `6.11.0-rc4-next-20240820`. The crash call chain was: `hrtimer_interrupt → sched_cfs_period_timer → distribute_cfs_runtime → unthrottle_cfs_rq`.

## Root Cause

The root cause is in the first `for_each_sched_entity(se)` loop inside `unthrottle_cfs_rq()` in `kernel/sched/fair.c`. The buggy code at the start of the loop was:

```c
if (se->on_rq) {
    SCHED_WARN_ON(se->sched_delayed);
    break;
}
enqueue_entity(qcfs_rq, se, ENQUEUE_WAKEUP);
```

This logic assumed that if an entity was already `on_rq`, it was in a healthy, fully-enqueued state and the walk could safely stop. The `SCHED_WARN_ON(se->sched_delayed)` was a defensive check asserting that a delayed entity should never be encountered here — but after the delayed dequeue feature was introduced, this assertion could legitimately fire.

When the assertion fires and the code breaks out of the loop, the hierarchy is left in an inconsistent state:

1. B's group entity has been re-enqueued into A's `cfs_rq` (the lower part of the walk succeeded).
2. A's group entity remains on the root `cfs_rq` with `sched_delayed = true` — a zombie state where the entity is technically "on" the rbtree but marked for removal.
3. The `h_nr_running` counters on A's and root's `cfs_rq` are updated (incremented by B's task count), but A's entity is still in the delayed state, so the hierarchy's accounting is inconsistent.
4. Subsequent scheduling operations on A's entity (e.g., `pick_next_entity` picking the delayed entity and calling `dequeue_entities` with `DEQUEUE_DELAYED`) encounter unexpected state, triggering further warnings like `"delay && se->sched_delayed"` in `dequeue_entity()`.

The specific mechanism that creates a `sched_delayed` group entity during the unthrottle window is:

1. Cgroup hierarchy: root → A → B (B has bandwidth limit), root → A → C (no bandwidth limit).
2. Task T1 in B and task T2 in C both run on the same CPU.
3. B's bandwidth is exhausted. `throttle_cfs_rq(B's cfs_rq)` dequeues B's entity from A's `cfs_rq` using `DEQUEUE_SLEEP | DEQUEUE_SPECIAL` (the `DEQUEUE_SPECIAL` flag prevents delayed dequeue for the throttle path specifically).
4. A's `cfs_rq` still has C's entity (T2 is running), so A's group entity remains on the root `cfs_rq` with `on_rq = true`.
5. T2 goes to sleep. `dequeue_entities()` walks up: T2 is dequeued from C's `cfs_rq`, then C's entity from A's `cfs_rq`. A's `cfs_rq` now has `h_nr_running = 0`.
6. Walking further up to A's entity in the root `cfs_rq`: the flags are `DEQUEUE_SLEEP` (note: `DEQUEUE_SPECIAL` was cleared by `flags &= ~(DEQUEUE_DELAYED | DEQUEUE_SPECIAL)` at line 7146 of the buggy code). If A's entity is NOT eligible (its vruntime exceeds the weighted average of the root `cfs_rq`), `dequeue_entity()` sets `se->sched_delayed = 1` and returns `false`.
7. A's entity is now on the root `cfs_rq` with `sched_delayed = true`, `on_rq = true`.
8. The bandwidth period timer fires, distributes runtime, and calls `unthrottle_cfs_rq(B's cfs_rq)`.
9. `unthrottle_cfs_rq` walks up from B's entity → enqueues B into A's `cfs_rq` → walks to A's entity → finds `on_rq = true`, `sched_delayed = true` → BUG.

The critical detail is that `dequeue_entities()` clears `DEQUEUE_SPECIAL` when propagating flags to parent entities (line 7146: `flags &= ~(DEQUEUE_DELAYED | DEQUEUE_SPECIAL)`), so parent group entities ARE subject to delayed dequeue during normal task sleep, unlike the throttle path which explicitly uses `DEQUEUE_SPECIAL`.

## Consequence

The immediate observable consequence is a kernel `WARNING` at the `SCHED_WARN_ON(se->sched_delayed)` line in `unthrottle_cfs_rq()`. The stack trace from the original report shows:

```
WARNING: CPU: 0 PID: 45927 at kernel/sched/fair.c:6049 unthrottle_cfs_rq+0x624/0x634
  unthrottle_cfs_rq+0x620/0x634
  distribute_cfs_runtime+0x3f0/0x51c
  sched_cfs_period_timer+0x170/0x348
  __hrtimer_run_queues+0x1bc/0x3d8
  hrtimer_interrupt+0x128/0x2fc
  timer_interrupt+0xf4/0x310
```

This is followed by a cascading failure. When the delayed entity is left in an inconsistent state, the scheduler's `pick_next_entity()` function can later pick it from the rbtree, discover `sched_delayed = true`, and call `dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED)`. This triggers the second warning observed in the original report:

```
delay && se->sched_delayed
WARNING: CPU: 8 PID: 4577 at kernel/sched/fair.c:5477 dequeue_entity+0x4b0/0x6d8
```

The commit author Mike Galbraith described these as "a couple terminal scenarios." The consequences can escalate to:

- **Scheduler corruption**: `h_nr_running` counts become inconsistent across the hierarchy, leading to incorrect load balancing decisions and potential assertion failures in other scheduler paths.
- **Task starvation**: If the group entity remains in the `sched_delayed` state indefinitely, tasks within the group may not be properly scheduled, leading to starvation.
- **Kernel panic/hang**: In severe cases, the cascading warnings and inconsistent state can trigger panics or soft lockups, especially in `PREEMPT_RT` or debug-enabled configurations.

The reporter confirmed the issue was reproducible on every LTP FS Stress run on their PowerPC POWER9 system with 6.11.0-rc7-next-20240910.

## Fix Summary

The fix replaces the `on_rq` check with explicit `sched_delayed` handling. The new code in the first `for_each_sched_entity(se)` loop of `unthrottle_cfs_rq()` is:

```c
/* Handle any unfinished DELAY_DEQUEUE business first. */
if (se->sched_delayed) {
    int flags = DEQUEUE_SLEEP | DEQUEUE_DELAYED;

    dequeue_entity(qcfs_rq, se, flags);
} else if (se->on_rq)
    break;
enqueue_entity(qcfs_rq, se, ENQUEUE_WAKEUP);
```

The logic now handles three cases:

1. **`se->sched_delayed` is true** (implies `se->on_rq` is also true): The entity is in a delayed-dequeue state. First, complete the delayed dequeue by calling `dequeue_entity()` with `DEQUEUE_SLEEP | DEQUEUE_DELAYED`. This fully removes the entity from the `cfs_rq`, setting `on_rq = false` and `sched_delayed = false`. Then fall through to `enqueue_entity()` with `ENQUEUE_WAKEUP` to re-enqueue the entity fresh, as if it were being woken up.

2. **`se->on_rq` is true but `se->sched_delayed` is false**: The entity is properly on the run queue. Break out of the loop (same as old behavior for the non-delayed case).

3. **Neither condition** (`se->on_rq` is false): The entity is off the run queue. Fall through to `enqueue_entity()` to re-enqueue it (same as old behavior).

This fix is correct because the dequeue-then-re-enqueue sequence effectively "resets" the entity to a clean woken-up state. The `ENQUEUE_WAKEUP` flag ensures proper placement (vruntime adjustment via `place_entity()`) and accounting. The approach mirrors how other scheduler paths (e.g., `enqueue_task_fair()`) handle `sched_delayed` entities — they call `requeue_delayed_entity()` which internally dequeues and re-enqueues.

## Triggering Conditions

The following conditions must ALL be met simultaneously:

1. **Kernel version v6.12-rc1 through v6.12-rc3** (inclusive): The bug requires commit `152e11f6df29` ("sched/fair: Implement delayed dequeue") to be present and commit `9b5ce1a37e904fac32d560668134965f4e937f6c` to be absent. The `DELAY_DEQUEUE` sched feature must be enabled (it is by default).

2. **CFS bandwidth throttling**: `CONFIG_CFS_BANDWIDTH=y` (default when `CONFIG_CGROUPS` and `CONFIG_FAIR_GROUP_SCHED` are enabled). At least one cgroup in the hierarchy must have a CPU bandwidth limit (`cpu.max` quota < period in cgroupv2, or `cpu.cfs_quota_us` < `cpu.cfs_period_us` in cgroupv1).

3. **Nested cgroup hierarchy of at least 3 levels**: root → A → B (where B has bandwidth limits) and root → A → C (sibling of B under A). The bug manifests at the A level (the intermediate group), not at the B level (the directly throttled group). The throttle path properly handles B's entity using `DEQUEUE_SPECIAL`, but the unthrottle walk-up encounters A's entity.

4. **A's group entity must be in `sched_delayed` state**: This requires:
   - A's `cfs_rq` must become empty (all child entities either throttled or dequeued) while B is throttled.
   - A's entity in the root `cfs_rq` must be NOT eligible per the EEVDF `entity_eligible()` check at the moment of dequeue. This means A's virtual runtime must be above the weighted average of the root `cfs_rq`. Practically, this means there must be at least one other entity on the root `cfs_rq` (e.g., another cgroup D) with lower vruntime.
   - The entity_eligible check is: `avg_vruntime >= entity_key(cfs_rq, se) * avg_load`. A fails this when A's vruntime exceeds the weighted average.

5. **Timing**: B must be throttled, then A must enter `sched_delayed` state, then B must be unthrottled — all before A's delayed entity is resolved by `pick_next_entity()`. The bandwidth period timer (default 100ms period, 5ms quota) governs the unthrottle timing. The window between A entering `sched_delayed` and B being unthrottled must overlap.

6. **Multiple CPUs**: CPU 0 is used for running the kSTEP driver. At least one additional CPU is needed for the scheduler to process tasks. QEMU should be configured with at least 2 CPUs.

The probability of hitting this bug in production depends on workload characteristics. Bandwidth-limited cgroups with nested hierarchies and fluctuating task counts (tasks frequently sleeping and waking) are most likely to trigger it. The LTP FS Stress test creates many processes under cgroups with bandwidth limits, making it a reliable trigger.

## Reproduce Strategy (kSTEP)

### Overview

The reproduction requires a 3-level cgroup hierarchy where the intermediate level's group entity enters `sched_delayed` state while a child cgroup is bandwidth-throttled. When the child cgroup is unthrottled, `unthrottle_cfs_rq()` walks up the hierarchy and encounters the delayed entity, triggering the bug.

### Step-by-step Plan

**1. Topology and cgroup setup:**

```c
// Minimum 2 CPUs: CPU 0 for driver, CPU 1 for test tasks
kstep_cgroup_create("A");
kstep_cgroup_create("A/B");
kstep_cgroup_create("A/C");
kstep_cgroup_create("D");

// B has a tight bandwidth limit: 5ms per 100ms period
kstep_cgroup_write("A/B", "cpu.max", "5000 100000");
```

The hierarchy is: root → A → B (bandwidth limited), root → A → C, root → D (reference group to create vruntime imbalance in root `cfs_rq`).

**2. Task creation and placement:**

```c
struct task_struct *t1 = kstep_task_create();  // In B (bandwidth limited)
struct task_struct *t2 = kstep_task_create();  // In C (will be blocked to empty A)
struct task_struct *t3 = kstep_task_create();  // In D (reference, keeps root cfs_rq populated)

kstep_task_pin(t1, 1, 1);
kstep_task_pin(t2, 1, 1);
kstep_task_pin(t3, 1, 1);

kstep_cgroup_add_task("A/B", t1->pid);
kstep_cgroup_add_task("A/C", t2->pid);
kstep_cgroup_add_task("D", t3->pid);
```

All tasks are pinned to CPU 1. T3 in D ensures the root `cfs_rq` has multiple entities, which is necessary for the `entity_eligible()` check to return false for A's entity.

**3. Wake tasks and consume bandwidth:**

```c
kstep_task_wakeup(t1);
kstep_task_wakeup(t2);
kstep_task_wakeup(t3);
```

All three tasks are now runnable on CPU 1. At the root `cfs_rq` level, there are two group entities: A (with T1 and T2 contributing to its load) and D (with T3).

**4. Tick until B's `cfs_rq` is throttled:**

Use `KSYM_IMPORT` and `internal.h` to access scheduler internals:

```c
struct cfs_rq *cfs_rq_b = t1->sched_task_group->cfs_rq[1];
for (int i = 0; i < 200; i++) {
    kstep_tick();
    if (cfs_rq_b->throttled)
        break;
}
```

After enough ticks, T1 consumes B's 5ms bandwidth quota, and B's `cfs_rq` on CPU 1 gets throttled. At this point, B's entity is dequeued from A's `cfs_rq` (with `DEQUEUE_SPECIAL`), but A's entity is still on the root `cfs_rq` because C's entity (T2 running) keeps A's `cfs_rq` populated.

**5. Block T2 to trigger A's delayed dequeue:**

```c
// Get A's entity in root cfs_rq for monitoring
struct sched_entity *se_a = t1->sched_task_group->parent->se[1];
struct cfs_rq *root_cfs_rq = &cpu_rq(1)->cfs;

kstep_task_block(t2);
```

When T2 is blocked with `DEQUEUE_SLEEP`, `dequeue_entities()` walks up:
- T2's entity removed from C's `cfs_rq`.
- C's entity removed from A's `cfs_rq` (A's `h_nr_running` drops to 0).
- A's entity in root `cfs_rq`: `dequeue_entity()` is called with `DEQUEUE_SLEEP` (note: `DEQUEUE_SPECIAL` was stripped by the flag clearing at line 7146). If A's entity is NOT eligible (A's vruntime > D's vruntime in root `cfs_rq`), it gets `sched_delayed = true`.

**6. Verify the delayed state:**

```c
if (se_a->sched_delayed && se_a->on_rq) {
    TRACE_INFO("A's entity is sched_delayed — proceed to unthrottle");
} else {
    // A was eligible — retry with more tick iterations to shift vruntime
    TRACE_INFO("A eligible — need more vruntime imbalance");
}
```

If A's entity did NOT get delayed dequeue (because it was still eligible), the driver should retry by ticking more to create vruntime imbalance. Alternatively, use `kstep_cgroup_set_weight("D", 10000)` to give D very high weight, making D's vruntime advance much slower and ensuring A's vruntime exceeds D's.

**7. Tick until unthrottle fires:**

```c
// Tick enough for the bandwidth period timer to fire (100ms period ~ 100 ticks at 1ms)
for (int i = 0; i < 150; i++) {
    kstep_tick();
    if (!cfs_rq_b->throttled)
        break;
}
```

When the bandwidth period timer fires, `distribute_cfs_runtime()` replenishes B's runtime and calls `unthrottle_cfs_rq(B's cfs_rq)`. The unthrottle walk encounters A's entity with `sched_delayed = true`.

**8. Detection / pass-fail criteria:**

On the **buggy kernel** (v6.12-rc1 through v6.12-rc3):
- `SCHED_WARN_ON(se->sched_delayed)` fires in `unthrottle_cfs_rq()`, producing a kernel warning in dmesg.
- Cascading warnings may follow in `dequeue_entity()`.
- The `h_nr_running` counts may become inconsistent.
- Detection: check for kernel warnings in the log (`cat data/logs/latest.log | grep "WARNING"`), or verify that `se_a->sched_delayed` is still `true` after unthrottle (it shouldn't be), or check `h_nr_running` consistency.

On the **fixed kernel** (v6.12-rc4+):
- `unthrottle_cfs_rq()` detects A's `sched_delayed` state, calls `dequeue_entity()` to complete the delayed dequeue, then calls `enqueue_entity()` with `ENQUEUE_WAKEUP` to re-enqueue A properly.
- No warnings, no inconsistent state.
- `se_a->sched_delayed` is `false` and `se_a->on_rq` is `true` after unthrottle.
- `h_nr_running` counts are consistent throughout the hierarchy.

```c
// After unthrottle, verify state
if (se_a->sched_delayed) {
    kstep_fail("A's entity still sched_delayed after unthrottle — BUG");
} else {
    kstep_pass("A's entity properly handled during unthrottle");
}
```

### Eligibility Control Strategy

The main challenge is ensuring A's entity is NOT eligible in the root `cfs_rq` when T2 goes to sleep. Two approaches:

**Approach A — Weight imbalance:** Set D's weight very high (`kstep_cgroup_set_weight("D", 10000)`) so D gets much more CPU time. D's vruntime advances slowly per unit of time due to high weight. A runs less but its vruntime advances faster per unit of runtime (default weight). After a few ticks, A's vruntime exceeds D's, making A not eligible.

**Approach B — Timing loop:** Use `entity_eligible(root_cfs_rq, se_a)` (accessed via `KSYM_IMPORT`) inside a tick loop. After B is throttled, tick until A's entity becomes not eligible, THEN block T2:

```c
for (int i = 0; i < 100; i++) {
    kstep_tick();
    if (!entity_eligible(root_cfs_rq, se_a)) {
        kstep_task_block(t2);
        break;
    }
}
```

**Approach C — Multiple attempts:** If the eligibility window is narrow, retry the entire sequence multiple times. Reset state by waking T2, letting bandwidth refill, and trying again.

### Guard Clause

The driver should be guarded with a version check since delayed dequeue only exists from v6.12:

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
```

### Expected kSTEP Modifications

No modifications to the kSTEP framework are needed. The driver uses existing APIs:
- `kstep_cgroup_create`, `kstep_cgroup_write`, `kstep_cgroup_add_task` for cgroup setup
- `kstep_task_create`, `kstep_task_pin`, `kstep_task_wakeup`, `kstep_task_block` for task management
- `kstep_tick`, `kstep_tick_repeat` for time advancement
- `internal.h` for accessing `cfs_rq->throttled`, `se->sched_delayed`, `se->on_rq`
- `KSYM_IMPORT(entity_eligible)` for checking eligibility (if needed)
