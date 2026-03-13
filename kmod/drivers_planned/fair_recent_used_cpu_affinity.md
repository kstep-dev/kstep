# Fair: Wrong CPU Tested Against Affinity in select_idle_sibling recent_used_cpu Path

**Commit:** `ae2ad293d6be143ad223f5f947cca07bcbe42595`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.5-rc2
**Buggy since:** v5.15-rc1 (introduced by `89aafd67f28c` "sched/fair: Use prev instead of new target as recent_used_cpu")

## Bug Description

The `select_idle_sibling()` function in the CFS scheduler is called during task wakeup to find an idle CPU for the waking task. One of its heuristics checks whether the task's "recently used CPU" (`p->recent_used_cpu`) is idle and could serve as a good placement candidate. This heuristic aims to improve cache reuse by preferring a CPU that the task ran on recently, even if it is not the immediately previous CPU (`prev`).

Commit `89aafd67f28c` optimized this code path by moving the `p->recent_used_cpu = prev` assignment to before the `if` block (previously it was inside the `if` block only on the success path). This refactoring preserved the `recent_used_cpu` value in a local variable before overwriting the struct field. However, the `cpumask_test_cpu()` call that verifies the candidate CPU is in the task's allowed CPU mask (`p->cpus_ptr`) was not updated to use the local variable — it still referenced `p->recent_used_cpu`, which by this point had already been overwritten with the value of `prev`.

As a result, the affinity check tests `prev` (the task's last-run CPU, now stored in `p->recent_used_cpu`) instead of `recent_used_cpu` (the actual candidate CPU being evaluated). This means the wrong CPU is validated against the task's `cpus_ptr` affinity mask.

The bug is subtle because in the common case `prev` and `recent_used_cpu` are both within the task's allowed CPUs. But when a task's CPU affinity has been narrowed (e.g., via `sched_setaffinity()` or cpuset constraints), the two values can diverge in their membership, leading to incorrect scheduling decisions.

## Root Cause

In `select_idle_sibling()`, the relevant code sequence (after commit `89aafd67f28c`, before the fix) is:

```c
/* Check a recently used CPU as a potential idle candidate: */
recent_used_cpu = p->recent_used_cpu;
p->recent_used_cpu = prev;          // <-- overwrites the struct field with prev
if (recent_used_cpu != prev &&
    recent_used_cpu != target &&
    cpus_share_cache(recent_used_cpu, target) &&
    (available_idle_cpu(recent_used_cpu) || sched_idle_cpu(recent_used_cpu)) &&
    cpumask_test_cpu(p->recent_used_cpu, p->cpus_ptr) &&   // <-- BUG: tests prev, not recent_used_cpu
    asym_fits_cpu(task_util, util_min, util_max, recent_used_cpu)) {
    return recent_used_cpu;
}
```

The local variable `recent_used_cpu` correctly holds the old value of `p->recent_used_cpu`. After the line `p->recent_used_cpu = prev`, the struct field `p->recent_used_cpu` equals `prev`. All the other conditions in the `if` statement correctly use the local `recent_used_cpu` variable — the cache-sharing check, the idle check, and the asymmetric capacity check all reference `recent_used_cpu`. Only the `cpumask_test_cpu()` call mistakenly references `p->recent_used_cpu` (which is now `prev`).

This creates two problematic scenarios:

**Scenario A (False positive — affinity violation):** If `prev` is in `p->cpus_ptr` but `recent_used_cpu` is NOT in `p->cpus_ptr`, the `cpumask_test_cpu()` check incorrectly passes. The function returns `recent_used_cpu` — a CPU the task is not allowed to run on. This can lead to the task being scheduled on a forbidden CPU, violating the task's affinity constraints set by `sched_setaffinity()` or cpuset.

**Scenario B (False negative — missed optimization):** If `prev` is NOT in `p->cpus_ptr` but `recent_used_cpu` IS in `p->cpus_ptr`, the check incorrectly fails. A valid idle CPU candidate is skipped, and the function falls through to more expensive idle-scanning heuristics (e.g., `select_idle_cpu()` or `select_idle_core()`). This degrades wakeup latency and may result in a suboptimal CPU placement.

Note that `prev` being outside `p->cpus_ptr` is possible when affinity has been changed while the task is sleeping — the task was last scheduled on `prev`, but a subsequent `sched_setaffinity()` call removed `prev` from the allowed set.

## Consequence

**Affinity violation (Scenario A):** The most severe consequence is that a task can be placed on a CPU outside its allowed affinity mask. This violates a fundamental kernel contract: if userspace (or another kernel subsystem) has restricted a task's CPU affinity, the scheduler must respect that restriction. An affinity violation can cause:
- Incorrect behavior in applications that rely on CPU pinning for correctness (e.g., DPDK, real-time applications, applications using CPU-local resources).
- Performance anomalies when tasks are placed on CPUs with different capacity, cache, or NUMA characteristics than expected.
- Potential data corruption in lock-free algorithms that rely on per-CPU affinity guarantees.

**Missed optimization (Scenario B):** When a valid idle `recent_used_cpu` is skipped, the scheduler falls through to the `select_idle_cpu()` scan, which iterates over CPUs in the LLC domain looking for an idle one. This is more expensive (higher wakeup latency) and may select a CPU with cold caches. The hit rate of the `recent_used_cpu` heuristic was measured by Mel Gorman at 57-85% for pipe workloads, so a significant fraction of wakeups could be degraded.

In practice, the bug most commonly manifests as Scenario B (missed optimizations) since most tasks don't have restricted affinity. Scenario A requires a specific sequence of affinity changes between wakeups, which is less common but more severe when it occurs.

## Fix Summary

The fix is a single-character change: replace `p->recent_used_cpu` with `recent_used_cpu` in the `cpumask_test_cpu()` call:

```c
-    cpumask_test_cpu(p->recent_used_cpu, p->cpus_ptr) &&
+    cpumask_test_cpu(recent_used_cpu, p->cpus_ptr) &&
```

After the fix, `cpumask_test_cpu()` correctly tests whether the *candidate CPU* (`recent_used_cpu`, the local variable holding the old `p->recent_used_cpu` value) is in the task's allowed CPU set. This is consistent with all the other conditions in the same `if` statement, which already use the local `recent_used_cpu` variable.

The fix is correct and complete because: (1) it ensures the affinity check validates the same CPU that would be returned, preventing affinity violations; (2) it does not change any other logic — the `p->recent_used_cpu = prev` assignment still happens before the `if` (preserving the optimization from commit `89aafd67f28c`); and (3) the local variable `recent_used_cpu` is still in scope and holds the correct value.

## Triggering Conditions

To trigger the bug, the following conditions must all be met:

1. **Multiple CPUs with shared LLC:** At least 3 CPUs (excluding CPU 0 which is reserved in kSTEP) that share a last-level cache, so that the `cpus_share_cache(recent_used_cpu, target)` check passes. A simple SMP system with 4+ CPUs in a single LLC domain suffices.

2. **Task with history on multiple CPUs:** A task must have run on at least two different CPUs across consecutive wakeups so that `recent_used_cpu != prev` and `recent_used_cpu != target`. Specifically:
   - On wakeup N, the task runs on CPU A. During `select_idle_sibling`, `p->recent_used_cpu` is set to `prev` of that wakeup.
   - On wakeup N+1, the task runs on CPU B (different from A). Now `p->recent_used_cpu` = A (from the previous wakeup's `prev`).
   - On wakeup N+2, `recent_used_cpu` (local) = A, while `prev` = B. After `p->recent_used_cpu = prev`, the struct field = B.

3. **CPU affinity restricts recent_used_cpu but allows prev (Scenario A):** Between wakeup N+1 and N+2, the task's CPU affinity is changed to exclude CPU A (the `recent_used_cpu`) but still include CPU B (which becomes `prev`). The buggy `cpumask_test_cpu(p->recent_used_cpu, p->cpus_ptr)` tests CPU B (which is allowed) instead of CPU A (which is not), so the check falsely passes and the function returns CPU A — an affinity violation.

4. **The recent_used_cpu must be idle:** CPU A (the `recent_used_cpu`) must be idle at the time of the wakeup, so that `available_idle_cpu(recent_used_cpu)` or `sched_idle_cpu(recent_used_cpu)` returns true. This can be ensured by not having other tasks running on that CPU.

5. **No short-circuit before the recent_used_cpu check:** The `prev` and `target` checks earlier in the function must not return first. This means: `prev` must not be idle (or must not share cache with target), and `target` must not already be the best choice. Alternatively, `prev == target` skips the prev-idle check, so having `prev == target` simplifies this.

The probability of triggering the bug in a production system is low in the common case (tasks rarely have dynamically-changing restricted affinity). However, in systems using cpusets, cgroup CPU affinity management, or applications that call `sched_setaffinity()` during operation, the bug can be triggered reliably with the right sequence of wakeup-block-affinity-change-wakeup cycles.

## Reproduce Strategy (kSTEP)

The following strategy uses kSTEP kthreads (for flexible mask-based affinity) to trigger the bug by demonstrating that the scheduler returns a CPU outside the task's allowed affinity mask (Scenario A).

### Setup

1. **QEMU Configuration:** 4 CPUs (CPU 0 reserved for driver, CPUs 1-3 available for tasks). All CPUs in a single LLC domain for cache sharing.

2. **Topology:** Use `kstep_topo_init()` and `kstep_topo_set_mc()` to place CPUs 0-3 in a single multi-core domain, ensuring `cpus_share_cache()` returns true for all pairs.

3. **Create a test kthread:** Use `kstep_kthread_create("test_task")` to create a CFS kthread. Initially bind it to CPUs 1-3 using `kstep_kthread_bind(p, mask)` with a mask containing CPUs {1, 2, 3}.

### Execution Sequence

4. **Wakeup 1 — Establish `recent_used_cpu`:**
   - Start the kthread with `kstep_kthread_start(p)`.
   - Tick a few times to let it get scheduled. The task will land on some CPU (say CPU 1).
   - Block the task with `kstep_kthread_block(p)`.
   - Tick to process the block.

5. **Wakeup 2 — Set `prev` to a different CPU:**
   - Temporarily bind the task to CPU 2 only using `kstep_kthread_bind(p, {2})`.
   - Wake the task using `kstep_kthread_syncwake(waker, p)` from CPU 0, or use another mechanism to trigger `try_to_wake_up`.
   - Tick to let it run on CPU 2. Now `p->recent_used_cpu` should be set during `select_idle_sibling`.
   - Block the task again.
   - Tick to process.

6. **Wakeup 3 — Change affinity to trigger bug:**
   - Now rebind the task to CPUs {2, 3} only (excluding CPU 1, which may be the `recent_used_cpu` from the first wakeup cycle, or whichever CPU was set).
   - Actually, to be precise: after wakeup 2, `p->recent_used_cpu = prev` where prev was whatever CPU the task was on before wakeup 2. We need to carefully track this.

### Detailed Control Flow

A more precise approach:

1. Create kthread T, bind to {1, 2, 3}, start it.
2. Use `kstep_kthread_bind(T, {1})` to force T onto CPU 1. Wake/tick. T runs on CPU 1.
3. Block T. Now `task_cpu(T) = 1`.
4. Bind T to {2}. Wake T. In `select_idle_sibling`: `prev = 1` (last CPU), `target` is computed. The function sets `p->recent_used_cpu = prev = 1` (since the original `recent_used_cpu` may have been something else or -1). T lands on CPU 2.
5. Block T. Now `task_cpu(T) = 2`, `p->recent_used_cpu = 1` (set during step 4's `select_idle_sibling`).
6. Bind T to {2, 3} (exclude CPU 1!). Make sure CPU 1 is idle (no tasks on it).
7. Wake T. In `select_idle_sibling`:
   - `prev = 2` (task's last CPU)
   - `recent_used_cpu` (local) = `p->recent_used_cpu` = 1
   - `p->recent_used_cpu` is then set to `prev = 2`
   - Check: `recent_used_cpu (1) != prev (2)` ✓
   - Check: `recent_used_cpu (1) != target` (likely 2, but could differ) — need `target != 1` ✓ if target is computed from wake_affine
   - Check: `cpus_share_cache(1, target)` ✓ (same LLC)
   - Check: `available_idle_cpu(1)` ✓ (CPU 1 is idle)
   - **BUG CHECK:** `cpumask_test_cpu(p->recent_used_cpu=2, p->cpus_ptr)` → tests CPU 2 → ✓ (CPU 2 is in {2,3})
   - **CORRECT CHECK:** `cpumask_test_cpu(recent_used_cpu=1, p->cpus_ptr)` → tests CPU 1 → ✗ (CPU 1 NOT in {2,3})
   - On **buggy kernel:** function returns CPU 1 → **affinity violation!**
   - On **fixed kernel:** check fails, function does NOT return CPU 1, falls through to find a CPU in {2, 3}.

### Detection

8. **After wakeup 3,** tick to let the scheduler place the task, then read `task_cpu(T)`:
   - On **buggy kernel:** `task_cpu(T)` may be 1, which is NOT in {2, 3}. Call `kstep_fail("task placed on CPU %d outside affinity", task_cpu(T))`.
   - On **fixed kernel:** `task_cpu(T)` should be 2 or 3 (within {2, 3}). Call `kstep_pass("task correctly placed on CPU %d", task_cpu(T))`.

9. **Alternative detection via KSYM_IMPORT:** Use `KSYM_IMPORT` to access `p->recent_used_cpu` directly after each wakeup and log the value of both the struct field and the actual CPU the task lands on. This provides detailed tracing even when the affinity check does not produce a visible violation.

### kSTEP Considerations

- **kstep_kthread_bind()** accepts a `const struct cpumask *`, so we can construct arbitrary bitmasks using `cpumask_clear()`, `cpumask_set_cpu()`, etc. This is essential for excluding specific CPUs (e.g., CPU 1) while including others (CPUs 2, 3).
- **kstep_kthread_syncwake()** triggers `try_to_wake_up()`, which calls `select_task_rq_fair()` → `select_idle_sibling()`, exercising the buggy code path.
- We need to ensure no other tasks are competing for CPUs 1-3 to keep them idle and make the test deterministic.
- The `on_tick_begin` callback can be used to log `task_cpu(T)` and `p->recent_used_cpu` at each tick for debugging.
- QEMU should be configured with at least 4 CPUs. The driver must not pin tasks to CPU 0.

### Expected Outcomes

- **Buggy kernel (v5.15-rc1 through v6.5-rc1):** The task is placed on CPU 1 despite CPU 1 being excluded from its cpus_ptr. The driver reports `kstep_fail()`.
- **Fixed kernel (v6.5-rc2+):** The `cpumask_test_cpu()` correctly rejects CPU 1 as a candidate. The task is placed on CPU 2 or 3. The driver reports `kstep_pass()`.
