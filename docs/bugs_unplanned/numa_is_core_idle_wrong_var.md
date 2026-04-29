# NUMA: is_core_idle() Checks Wrong CPU Variable in SMT Sibling Loop

**Commit:** `1c6829cfd3d5124b125e6df41158665aea413b35`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.15-rc1
**Buggy since:** v5.7-rc1 (introduced by `ff7db0bf24db` "sched/numa: Prefer using an idle CPU as a migration target instead of comparing tasks")

## Bug Description

The function `is_core_idle()` in the NUMA balancing code is intended to determine whether all SMT siblings of a given CPU are idle. It iterates over all CPUs in the SMT mask of the input `cpu`, skips the input CPU itself, and checks whether each sibling is idle. However, due to a copy-paste or variable naming error introduced in commit `ff7db0bf24db`, the function passes the original `cpu` argument to `idle_cpu()` instead of the loop variable `sibling`.

This means the function never actually checks the idle state of any sibling CPU. Instead, it repeatedly checks whether the original `cpu` itself is idle, once for every sibling. The result is that `is_core_idle()` returns `true` whenever the input CPU is idle (regardless of sibling state) and returns `false` whenever the input CPU is busy (regardless of sibling state). This completely defeats the purpose of the function.

The `is_core_idle()` function is called from `numa_idle_core()`, which in turn is called from `update_numa_stats()` during NUMA migration target selection. When the NUMA balancer scans a destination node's CPUs to find an idle core, the incorrect `is_core_idle()` can falsely report that a core is fully idle when one of its SMT siblings is actually running a task, or conversely report a core as busy when all siblings are actually idle but the initially idle CPU happens to have been selected.

The bug existed from v5.7-rc1 (when `is_core_idle()` was first introduced) through v5.14, and was fixed in v5.15-rc1. It affects all systems with `CONFIG_SCHED_SMT` and `CONFIG_NUMA` enabled.

## Root Cause

The root cause is a trivial variable name error in the `is_core_idle()` function. The buggy code reads:

```c
static inline bool is_core_idle(int cpu)
{
#ifdef CONFIG_SCHED_SMT
    int sibling;

    for_each_cpu(sibling, cpu_smt_mask(cpu)) {
        if (cpu == sibling)
            continue;

        if (!idle_cpu(cpu))     // BUG: should be idle_cpu(sibling)
            return false;
    }
#endif

    return true;
}
```

The `for_each_cpu()` macro iterates the loop variable `sibling` over all CPUs in the SMT mask of `cpu`. The intent is to skip the `cpu` itself (via the `continue` statement) and then check each remaining sibling for idleness. However, the `idle_cpu()` call on line 1515 erroneously passes `cpu` instead of `sibling`.

This creates two distinct failure modes depending on whether `cpu` is idle:

1. **When `cpu` is idle:** `idle_cpu(cpu)` returns true for every iteration, so the loop body never returns `false`. The function falls through and returns `true`, regardless of whether any sibling is running a task. This causes false positives — cores are reported as idle when they are not.

2. **When `cpu` is busy:** `idle_cpu(cpu)` returns false on the first iteration (after skipping `cpu` itself), and the function immediately returns `false`. This happens regardless of whether all siblings are actually idle. This causes false negatives — cores are reported as busy when they are fully idle.

The call chain is: `update_numa_stats()` → `numa_idle_core()` → `is_core_idle()`. In `update_numa_stats()`, the function scans all CPUs on a NUMA node. For each idle CPU found, it calls `numa_idle_core()` to check if the entire core is idle (all SMT siblings idle). If so, the idle core is cached in `ns->idle_cpu` for preferential selection as a NUMA migration target.

With the bug, the idle core detection is unreliable. An idle CPU on a partially-busy core may be flagged as an idle core (because the CPU being checked is idle, so `is_core_idle()` returns true). Conversely, in the less common scenario where the function is called for a busy CPU, a fully idle core would be missed.

## Consequence

The primary consequence is suboptimal NUMA migration target selection on SMT systems. When the NUMA balancer decides to migrate a task to a different NUMA node, it prefers to place the task on an idle core (all SMT siblings idle) rather than an idle HT sibling on a partially-busy core. This preference exists to avoid cache contention and resource sharing between SMT siblings, which degrades performance.

With the bug, `is_core_idle()` may falsely report a partially-busy core as fully idle. This causes the NUMA balancer to select a CPU that shares its core with a running task, believing it is an idle core. The migrated task then competes with the existing task for shared core resources (L1/L2 cache, execution units, memory bandwidth), resulting in degraded performance for both tasks. On workloads that are sensitive to cache locality and CPU resource sharing (e.g., database workloads, HPC applications, memory-intensive NUMA-aware applications), this can lead to measurable throughput regressions.

Conversely, the bug can also cause the NUMA balancer to miss truly idle cores when the checked CPU happens to be busy, though this is less likely since `update_numa_stats()` only calls `numa_idle_core()` for CPUs that pass the `idle_cpu()` check earlier. The net effect is that the idle core preference optimization introduced in `ff7db0bf24db` is effectively broken on all SMT systems, reducing the quality of NUMA placement decisions for the entire duration the bug existed (v5.7 through v5.14).

## Fix Summary

The fix is a one-line change that replaces `idle_cpu(cpu)` with `idle_cpu(sibling)` in the loop body of `is_core_idle()`:

```c
-       if (!idle_cpu(cpu))
+       if (!idle_cpu(sibling))
            return false;
```

After the fix, the function correctly checks each SMT sibling for idleness. If any sibling is not idle, the function returns `false` (the core is not fully idle). If all siblings are idle (or the CPU has no siblings), the function returns `true`.

This fix is correct and complete because it restores the intended semantics of the function: iterate over all SMT siblings of the given CPU (excluding the CPU itself) and return `false` if any sibling is running a task. The fix was acked by Mel Gorman (who authored the original commit that introduced the bug) and Peter Zijlstra (the scheduler maintainer). No additional changes were needed because the rest of the call chain (`numa_idle_core()`, `update_numa_stats()`, `task_numa_find_cpu()`) correctly uses the return value of `is_core_idle()`.

## Triggering Conditions

To trigger the bug, the following conditions must be met:

- **CONFIG_SCHED_SMT** must be enabled (and the system must have SMT/hyperthreading hardware with at least 2 threads per core).
- **CONFIG_NUMA** must be enabled with at least 2 NUMA nodes.
- **NUMA balancing** must be active (`kernel.numa_balancing = 1`), which is the default on most distributions.
- A task must be eligible for NUMA migration, meaning it has accumulated sufficient NUMA fault statistics indicating it would benefit from running on a different node. This requires the task to have an `mm_struct` (it must be a userspace process, not a kernel thread) and to have been running long enough for the NUMA scanning mechanism to collect page fault data.
- The destination NUMA node must have at least one idle CPU on an SMT core where at least one sibling is busy. This is the condition where `is_core_idle()` produces an incorrect result (false positive). This is a common scenario on lightly-to-moderately loaded systems.

The bug is deterministic — it always produces incorrect results when the conditions are met. There is no race condition or timing sensitivity. However, the impact on scheduling decisions depends on the workload: the bug only matters when the NUMA balancer's choice of idle core vs. idle sibling makes a difference to performance.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for two independent reasons:

### 1. Kernel Version Too Old

The buggy kernel at commit `1c6829cfd3d5~1` has `LINUX_VERSION_CODE` corresponding to v5.13.0-rc6 (as shown in its Makefile: `VERSION = 5`, `PATCHLEVEL = 13`, `SUBLEVEL = 0`, `EXTRAVERSION = -rc6`). kSTEP supports Linux v5.15 and newer only. The commit went through Peter Zijlstra's tip/sched/core branch based on v5.13-rc6, and was merged into mainline as part of v5.15-rc1. There is no mainline kernel version >= v5.15 that contains this bug — the fix is present in all v5.15+ kernels.

### 2. Requires Real Userspace Processes with NUMA Memory Access

Even if the kernel version were supported, this bug fundamentally requires real userspace processes with `mm_struct` and NUMA memory access patterns. The NUMA balancing mechanism that invokes `is_core_idle()` is triggered through the following chain:

- `task_tick_numa()` is called from the scheduler tick for tasks with `mm_struct`
- This eventually triggers `task_numa_work()`, which scans VMAs and changes PTE protection bits to trigger NUMA page faults
- NUMA page faults call `task_numa_fault()` to accumulate per-node access statistics
- When sufficient statistics are gathered, `task_numa_migrate()` is called to evaluate migration
- `task_numa_migrate()` calls `update_numa_stats()` → `numa_idle_core()` → `is_core_idle()`

kSTEP creates kernel threads (kthreads) which have no `mm_struct`, no VMAs, and no page tables. The NUMA scanning mechanism (`task_numa_work()`) explicitly checks for `current->mm` and returns early if it is NULL. Therefore, kSTEP tasks can never trigger NUMA migration, and `is_core_idle()` will never be called through this code path.

### 3. What Would Be Needed

To reproduce this bug, kSTEP would need:
- Support for Linux kernel v5.7 through v5.14 (pre-v5.15)
- The ability to create real userspace processes with `mm_struct` and virtual memory areas (VMAs)
- A real or emulated multi-node NUMA topology with actual memory access latency differences (to trigger NUMA page faults)
- Sufficient runtime for the NUMA scanning heuristic to collect page fault statistics and trigger migration

These are fundamental architectural requirements that are far beyond minor kSTEP extensions.

### 4. Alternative Reproduction Methods

Outside of kSTEP, the bug can be reproduced on any real NUMA system with SMT (e.g., a dual-socket Intel Xeon with hyperthreading) running a kernel between v5.7 and v5.14:

1. Boot with `numa_balancing=1` (default)
2. Run a memory-intensive multi-threaded workload that accesses memory on remote NUMA nodes (e.g., `numactl --membind=0 taskset -c 4-7 sysbench memory run` where CPUs 4-7 are on NUMA node 1)
3. Monitor NUMA migration decisions via `/proc/vmstat` (`numa_hint_faults`, `numa_pages_migrated`) or tracepoints (`sched:sched_move_numa`, `sched:sched_stick_numa`)
4. Observe that tasks are placed on SMT siblings of busy cores instead of truly idle cores, as evidenced by the `idle_cpu` field in `struct numa_stats` pointing to partially-busy cores

Alternatively, one can directly verify the bug by reading the source code — it is a straightforward variable-name error with no ambiguity about its incorrectness.
