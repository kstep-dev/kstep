# NUMA: Wrong argument to adjust_numa_imbalance in task_numa_find_cpu

**Commit:** `233e7aca4c8a2c764f556bba9644c36154017e7f`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.10-rc1
**Buggy since:** v5.7-rc1 (commit `fb86f5b21192` "sched/numa: Use similar logic to the load balancer for moving between domains with spare capacity")

## Bug Description

The NUMA balancer in the Linux scheduler decides whether to migrate a task to its preferred NUMA node based on memory access patterns. When a destination NUMA node has spare capacity (`node_has_spare`), the function `task_numa_find_cpu()` computes whether moving the task would create an imbalance that the regular CPU load balancer would later undo. This imbalance check calls `adjust_numa_imbalance()`, which allows small imbalances (up to 2 tasks) to be tolerated for a pair of communicating tasks on an almost-idle domain.

The bug is that `task_numa_find_cpu()` passes `src_running` (the source node's running count minus one) to `adjust_numa_imbalance()`, whereas it should pass `dst_running` (the destination node's running count plus one). This causes the function to check the wrong thing: instead of asking "will the destination domain be almost idle after the move?" it asks "is the source domain almost idle after the move?" This is inconsistent with how the load balancer uses the same function in `calculate_imbalance()`, where it passes `busiest->sum_nr_running` — the count of the domain that would have more tasks.

The inconsistency means the NUMA balancer and load balancer can disagree on whether a task distribution is balanced. The NUMA balancer may allow a migration that the load balancer considers imbalanced (and will reverse), or it may block a migration that the load balancer would have permitted. This creates a feedback loop of unnecessary migrations and suboptimal task placement, particularly when workloads use only a fraction of available CPUs across NUMA nodes.

## Root Cause

The function `adjust_numa_imbalance()` is designed to tolerate small imbalances between NUMA domains. It accepts an `imbalance` value and a running count, and returns 0 (meaning "no significant imbalance") if the running count is at or below a threshold of 2. The intent is that a pair of communicating tasks on an almost-idle node should not trigger rebalancing.

In `task_numa_find_cpu()`, when the destination node has spare capacity, the code computes:
```c
src_running = env->src_stats.nr_running - 1;  // after removing task
dst_running = env->dst_stats.nr_running + 1;  // after adding task
imbalance = max(0, dst_running - src_running);
imbalance = adjust_numa_imbalance(imbalance, src_running);  // BUG: should be dst_running
```

The `imbalance` variable represents how many more tasks the destination would have compared to the source after the move. The bug is on the last line: `src_running` is passed instead of `dst_running`. The function `adjust_numa_imbalance()` then checks whether `src_running <= 2`, and if so, returns 0 (allowing the move). But the correct check should be whether `dst_running <= 2`, which would mean the destination is almost idle and the imbalance is tolerable.

Consider a concrete example from the commit message: the destination node will have `dst_running = 3` tasks after the move, and the source will have `src_running = 1`. The computed `imbalance = max(0, 3 - 1) = 2`. With the buggy code, `adjust_numa_imbalance(2, 1)` is called. Since `src_running = 1 <= 2`, it returns 0, meaning "no imbalance" — so the move is allowed. But this is wrong: the destination would have 3 tasks and the source only 1, which is a real imbalance that the load balancer would try to correct.

With the fix, `adjust_numa_imbalance(2, 3)` is called. Since `dst_running = 3 > 2`, the function returns 2 (the actual imbalance), and the move is blocked. This is correct: the move would create an imbalance that the load balancer would reverse.

In the load balancer's `calculate_imbalance()`, when the local group has spare capacity and both groups have similar types, the call is:
```c
env->imbalance = adjust_numa_imbalance(env->imbalance, busiest->sum_nr_running);
```
Here, `busiest->sum_nr_running` is the count of the domain with more tasks — analogous to `dst_running` in the NUMA case (the domain that would have more tasks after the move). The fix makes the NUMA balancer consistent with this logic.

## Consequence

The observable impact of this bug is suboptimal task placement on NUMA systems, manifesting as performance degradation for workloads that utilize a fraction of available CPUs. The bug causes two types of incorrect behavior:

1. **Unnecessary NUMA migrations allowed**: When the source node is almost idle (1-2 tasks), the buggy code always clears the imbalance to 0, allowing the NUMA balancer to migrate a task to its preferred node even when doing so creates a significant task count imbalance. The load balancer then detects this imbalance and migrates the task back, causing a ping-pong effect that wastes CPU time and pollutes caches.

2. **Correct NUMA migrations blocked**: When the source node has many tasks but the destination is almost idle (1-2 tasks after the move), the buggy code reports a non-zero imbalance even though the move would be perfectly safe. This prevents tasks from reaching their preferred NUMA node, increasing remote memory access latency.

Benchmark results from the commit message show that fixing this bug improves throughput by 20-50% at 1/3 system utilization on a 2-socket 80-CPU system running specjbb 2005. At full utilization, the results are mixed (±5%), but overall the fix is considered beneficial. The bug particularly affects parallel workloads (e.g., NAS benchmarks, specjbb) where not all available parallelism is active at once, creating the "almost idle source node" condition that triggers the incorrect logic.

## Fix Summary

The fix is a single-argument change in `task_numa_find_cpu()`: the call `adjust_numa_imbalance(imbalance, src_running)` is changed to `adjust_numa_imbalance(imbalance, dst_running)`. Additionally, the function signature and parameter name are updated from `src_nr_running` to `nr_running` to reflect that the parameter's semantics are not tied to a specific source or destination.

Specifically, the fix changes the semantics of the NUMA imbalance check from "allow the move if the source domain is almost idle" to "allow the move if the destination domain would be almost idle after the move." This makes the check consistent with the load balancer's usage in `calculate_imbalance()`, where the function is called with the busiest group's running count. After the fix, both the NUMA balancer and the load balancer agree on what constitutes a tolerable imbalance, preventing the migration ping-pong that caused performance degradation.

The fix is correct and complete because `adjust_numa_imbalance()` is a heuristic that says "an imbalance is acceptable if the group with more tasks has at most 2 running tasks." In the NUMA case, after the proposed move, the group with more tasks is the destination (since `imbalance = max(0, dst_running - src_running)` means `dst_running >= src_running`). Therefore, `dst_running` is the correct argument to determine whether the imbalance is small enough to tolerate.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

- **NUMA topology**: The system must have at least 2 NUMA nodes. The bug manifests on multi-socket systems where tasks access memory on remote nodes.
- **NUMA balancing enabled**: `CONFIG_NUMA_BALANCING=y` and `/proc/sys/kernel/numa_balancing` set to 1 (default on most distributions).
- **Spare capacity on destination node**: The destination NUMA node must be classified as `node_has_spare` by `numa_classify()`, meaning `nr_running < weight` (fewer running tasks than CPUs) or the node has excess compute capacity relative to utilization.
- **Almost-idle source node (buggy trigger)**: The source node must have at most 3 tasks running (so that `src_running = nr_running - 1 <= 2`). This is the condition that incorrectly causes `adjust_numa_imbalance()` to return 0.
- **Imbalance present after move**: After the simulated move, `dst_running > src_running`, meaning the destination would have more tasks than the source. The absolute difference must be positive.
- **Task with preferred node different from current node**: The task must have been running long enough for the NUMA fault scanning to identify a preferred node that differs from the current node.

The typical scenario is a workload that uses about 1/3 of available CPUs across a multi-socket system. Some tasks have their memory allocated on a remote NUMA node and the NUMA balancer wants to migrate them to their preferred node. The source node has only 1-2 other tasks, so `src_running - 1` is 0 or 1, which triggers the buggy path.

The bug is deterministic given the right task distribution: any time `task_numa_find_cpu()` is called with `src_running <= 2` and `dst_running > 2`, the wrong decision will be made. The probability of hitting this depends on workload characteristics and system topology.

## Reproduce Strategy (kSTEP)

This bug CANNOT be reproduced with kSTEP for two independent reasons:

### 1. Kernel Version Too Old

The fix was merged into v5.10-rc1, and the bug existed from v5.7-rc1 through v5.9.x. kSTEP supports Linux v5.15 and newer only. The buggy kernel (`233e7aca4c8a~1`) is in the v5.9-v5.10 timeframe, which is before v5.15 and outside kSTEP's supported range. The kSTEP framework, build system, and kernel module infrastructure may not be compatible with kernels in this version range.

### 2. NUMA Balancing Requires Real Userspace Processes

Even if the kernel version were supported, this bug cannot be reproduced with kSTEP because NUMA balancing fundamentally requires real userspace processes with `mm_struct` and Virtual Memory Areas (VMAs). The NUMA balancing code path that triggers the bug is:

1. `task_numa_work()` scans a task's VMAs and marks pages with `PROT_NONE`
2. When the task accesses those pages, `do_numa_page()` handles the page fault
3. `task_numa_fault()` is called, updating `p->numa_faults[]` statistics
4. Periodically, `task_numa_placement()` analyzes fault statistics and determines `p->numa_preferred_nid`
5. If the preferred node differs from the current node, `task_numa_migrate()` is called
6. `task_numa_find_cpu()` is called within `task_numa_migrate()` to find the best CPU on the preferred node
7. Inside `task_numa_find_cpu()`, the buggy call to `adjust_numa_imbalance()` occurs

kSTEP uses kernel threads created via `kstep_kthread_create()` or `kstep_task_create()`. These kernel threads do not have an `mm_struct`, do not have VMAs, and cannot generate NUMA page faults. Without the NUMA fault scanning infrastructure, `task_numa_placement()` and `task_numa_migrate()` are never invoked, so the buggy code path is unreachable.

### 3. Static Functions Cannot Be Called Directly

The buggy function `task_numa_find_cpu()` is declared `static` in `kernel/sched/fair.c`. Even with kSTEP's `KSYM_IMPORT()` capability, static functions may not appear in the kernel symbol table (depending on configuration). And even if the function could be called, setting up a valid `task_numa_env` structure requires accurate `numa_stats` populated by `update_numa_stats()`, which itself reads per-CPU runqueue data that reflects real task distribution across NUMA nodes.

### 4. What Would Be Needed in kSTEP

To support this type of bug, kSTEP would need fundamental additions:
- **Userspace process simulation**: The ability to create tasks with `mm_struct` and VMAs, with memory allocated on specific NUMA nodes. This would require a fake page fault mechanism to simulate NUMA access patterns.
- **NUMA fault injection**: A way to inject `task_numa_fault()` calls or directly set `p->numa_faults[]` and `p->numa_preferred_nid` to simulate the output of NUMA fault scanning.
- **NUMA memory placement**: Actual NUMA memory allocation is needed to make the NUMA balancer treat tasks as having memory affinity to specific nodes.

These are not minor extensions — they require adding an entirely new subsystem (userspace memory simulation) to kSTEP.

### 5. Alternative Reproduction Methods

Outside kSTEP, this bug can be reproduced on a real multi-socket NUMA system by:
1. Booting a kernel between v5.7 and v5.9 with `CONFIG_NUMA_BALANCING=y`
2. Running a parallel workload (e.g., specjbb 2005 with warehouses ≈ 1/3 of CPU count) on a 2-socket system
3. Monitoring NUMA migrations via `/proc/vmstat` (`numa_hint_faults`, `numa_pages_migrated`) and task placement via `numastat`
4. Comparing throughput and migration counts between buggy and fixed kernels
5. Using `perf stat -e sched:sched_migrate_task` to observe migration ping-pong patterns
