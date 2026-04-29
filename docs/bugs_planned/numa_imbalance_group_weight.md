# NUMA: Inconsistent NUMA Imbalance Calculations Use Wrong Group and Weight

**Commit:** `2cfb7a1b031b0e816af7a6ee0c6ab83b0acdf05a`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.18-rc1
**Buggy since:** v5.11-rc1 (introduced by `7d2b5dd0bcc4` "sched/numa: Allow a floating imbalance between NUMA nodes")

## Bug Description

The Linux kernel's NUMA-aware load balancer allows a small imbalance between NUMA nodes so that communicating task pairs are not unnecessarily pulled apart. This tolerance is controlled by the `allow_numa_imbalance()` and `adjust_numa_imbalance()` functions in `kernel/sched/fair.c`. Commit `7d2b5dd0bcc4` introduced the core logic: if the destination node is lightly loaded (fewer than 25% of CPUs busy), a small imbalance (up to `NUMA_IMBALANCE_MIN = 2` tasks) is permitted. This prevents the periodic load balancer from splitting communicating task pairs across NUMA nodes, which would degrade locality.

However, the implementation of this feature had three distinct inconsistencies across the three call sites that invoke the imbalance check: `task_numa_find_cpu()`, `find_idlest_group()`, and `calculate_imbalance()`. The reference implementation in `task_numa_find_cpu()` correctly uses the *destination* node's running task count (incremented by 1 to account for the task being moved) and the destination node's weight (number of CPUs). The other two call sites deviated from this in different ways, leading to incorrect NUMA balancing decisions during both task wakeup placement and periodic load balancing.

These inconsistencies caused the load balancer to make incorrect decisions about whether to migrate tasks across NUMA nodes. On machines with multiple NUMA nodes and a moderate number of tasks (especially when the number of running tasks is near the 25% threshold), the buggy code would either over-eagerly pull tasks across nodes (breaking up communicating pairs and harming locality) or under-eagerly retain tasks on a node (preventing proper load spreading). The practical impact was measurable: benchmarks like STREAM (memory bandwidth), Coremark (CPU/cache), NPB-ep (parallel computation), and specjbb (throughput) showed significant performance regressions, particularly on AMD Zen3 systems with multiple LLCs per NUMA node.

## Root Cause

There are three specific bugs, all in `kernel/sched/fair.c`:

**Bug 1: Wrong weight in `find_idlest_group()` (wakeup placement path)**

In the `group_has_spare` case within `find_idlest_group()`, the buggy code was:
```c
if (allow_numa_imbalance(local_sgs.sum_nr_running, sd->span_weight))
    return NULL;
```
This used `sd->span_weight`, which is the weight of the *entire* scheduling domain (spanning all NUMA nodes), instead of `local_sgs.group_weight`, which is the weight of just the local group (one NUMA node). For a 2-node system with 8 CPUs per node, `sd->span_weight = 16` but `local_sgs.group_weight = 8`. The 25% threshold would be `16 >> 2 = 4` (buggy) vs `8 >> 2 = 2` (correct). This made the imbalance tolerance far too generous, keeping tasks on the local node even when the node was becoming relatively loaded.

**Bug 2: Missing +1 in both `find_idlest_group()` and `calculate_imbalance()`**

The `task_numa_find_cpu()` reference implementation accounts for the task being moved by computing `dst_running = env->dst_stats.nr_running + 1`. Neither `find_idlest_group()` nor `calculate_imbalance()` included this +1 adjustment. This means the imbalance check was evaluated against the *current* state rather than the *post-move* state. The task about to be placed or migrated was not counted, making the threshold check slightly too permissive.

**Bug 3: Wrong group in `calculate_imbalance()` (periodic load balancing path)**

In `calculate_imbalance()`, the buggy code was:
```c
env->imbalance = adjust_numa_imbalance(env->imbalance,
    busiest->sum_nr_running, busiest->group_weight);
```
This passed the `busiest` (source) group's `sum_nr_running` and `group_weight` to `adjust_numa_imbalance()`. But the intent is to check whether the *destination* (local) group will be lightly loaded after receiving the migrated task. The reference in `task_numa_find_cpu()` correctly uses `dst_running` and `env->dst_stats.weight`. Using the source group's metrics is semantically wrong: the check should ask "will the destination remain lightly loaded after receiving this task?" not "is the source lightly loaded?"

The function `allow_numa_imbalance()` also had a minor type inconsistency: its parameters were typed `int` but conceptually represent unsigned counts. The fix changes them to `unsigned int` and removes the misleading `dst_` prefix from parameter names, since the function is now correctly used for both source and destination contexts.

## Consequence

The primary consequence is **suboptimal task placement across NUMA nodes**, leading to measurable performance degradation. The bugs do not cause crashes, hangs, or data corruption, but they do cause the scheduler to make incorrect migration decisions.

**When the threshold is too permissive (Bug 1 and Bug 2):** Tasks are kept on the local NUMA node even when the node is becoming moderately loaded. This can cause multiple independent tasks to share cache (LLC) and memory bandwidth on the same node while leaving another node nearly idle. On AMD Zen3 machines with multiple LLCs per NUMA node, this effect is amplified: the STREAM benchmark showed up to **272% degradation** because parallel threads contended for the same LLCs instead of spreading across the machine. Coremark showed **10% degradation** in mean scores. NPB-ep showed **18% degradation**. These are not minor regressions; they represent fundamental misplacement of work.

**When the threshold is too strict (Bug 3):** The source group's higher task count is compared against the threshold instead of the destination's lower count. In `adjust_numa_imbalance()`, this can cause the function to *not* suppress the imbalance (returning the non-zero imbalance value), leading the load balancer to pull tasks across NUMA nodes unnecessarily. This breaks up communicating task pairs, increasing cross-node memory traffic and harming workloads like netperf or tbench that benefit from locality. For workloads like specjbb with varying thread counts, this manifested as significant variability: standard deviation improved by up to **96%** with the fix, indicating the buggy code caused erratic scheduling decisions.

## Fix Summary

The fix makes all three call sites consistent with the reference implementation in `task_numa_find_cpu()`:

1. **`allow_numa_imbalance()` signature**: Parameters changed from `int dst_running, int dst_weight` to `unsigned int running, unsigned int weight`. The `dst_` prefix is removed because the function is now generic (it checks whether a running count is below 25% of a weight, regardless of whether it's source or destination).

2. **`find_idlest_group()` call site**: Changed from `allow_numa_imbalance(local_sgs.sum_nr_running, sd->span_weight)` to `allow_numa_imbalance(local_sgs.sum_nr_running + 1, local_sgs.group_weight)`. This fixes both the wrong weight (domain weight → group weight) and the missing +1 (now accounts for the task being placed). The comment is also updated to clarify the intent: the task stays local only "if the number of running tasks would remain below threshold where an imbalance is allowed."

3. **`calculate_imbalance()` call site**: Changed from `adjust_numa_imbalance(env->imbalance, busiest->sum_nr_running, busiest->group_weight)` to `adjust_numa_imbalance(env->imbalance, local->sum_nr_running + 1, local->group_weight)`. This fixes both the wrong group (busiest → local) and the missing +1 (now accounts for the task being pulled to the local group).

The fix is correct and complete because all three code paths now use the same semantic: "given the running task count that the destination group *would have after the move*, and the destination group's CPU count, is the destination still lightly loaded enough to tolerate an imbalance?" This matches the reference implementation in `task_numa_find_cpu()`, which was the only correct call site before the fix.

## Triggering Conditions

To trigger the bug, the following conditions must be met:

- **NUMA topology**: The system must have at least 2 NUMA nodes with multiple CPUs per node. The SD_NUMA flag must be set on the top-level scheduling domain. The bug is more impactful with larger groups (8+ CPUs per node), because the threshold difference between the buggy and correct weight values is larger.

- **`group_has_spare` classification**: Both the local and busiest scheduling groups must be classified as `group_has_spare`, meaning they have idle CPUs and are not overloaded or fully busy. This is the most common state when the system is moderately loaded with fewer tasks than CPUs.

- **Task count near the 25% threshold**: The number of running tasks on a node should be near `group_weight >> 2` (25% of CPUs). For an 8-CPU node, this is 2 tasks. For a 16-CPU node, this is 4 tasks. When task counts are well below or well above this threshold, the buggy and correct code paths converge to the same decision.

- **For Bug 1 (`find_idlest_group`)**: A CFS task must wake up and go through the slow wakeup path in `select_task_rq_fair()`. This happens when `wake_affine()` doesn't apply or when `SD_BALANCE_FORK`/`SD_BALANCE_EXEC` is set. The waking task must be eligible for placement on a NUMA domain (not pinned to a single node).

- **For Bug 3 (`calculate_imbalance`)**: The periodic load balancer must run at the NUMA domain level. This happens after the NUMA domain's balance interval expires (typically several seconds at default settings). The load balancer CPU must be on the local (destination) node, and the busiest group must be on a remote node.

- **Unpinned tasks**: Tasks must not be pinned to specific CPUs via `sched_setaffinity()`, otherwise the load balancer cannot migrate them regardless of the imbalance calculation.

The bug is **deterministic** given the right task count and topology: for a specific configuration, the buggy code will consistently make a different decision than the fixed code. There is no race condition or timing sensitivity beyond the load balancer interval.

## Reproduce Strategy (kSTEP)

The bug can be reproduced using kSTEP by exploiting the `calculate_imbalance()` code path (Bug 3), which is the most reliably triggerable via periodic load balancing. The `find_idlest_group()` path (Bug 1) is also potentially triggerable but depends on the wakeup slow path being taken, which is harder to control.

### Target Code Path: `calculate_imbalance()` in Periodic Load Balance

**Step 1: QEMU and Topology Setup**

Configure QEMU with 16 CPUs and 2 NUMA nodes (8 CPUs per node). Use `kstep_topo_init()` and `kstep_topo_set_node()` to define the NUMA topology:
```c
kstep_topo_init();
const char *nodes[] = {"0-7", "8-15"};
kstep_topo_set_node(nodes, 2);
kstep_topo_apply();
```
This creates SD_NUMA scheduling domains spanning both nodes, each with `group_weight = 8` and `sd->span_weight = 16`.

**Step 2: Task Creation and Placement**

Create 2 CFS tasks and initially pin them to node 1 CPUs (8-15):
```c
struct task_struct *t1 = kstep_task_create();
struct task_struct *t2 = kstep_task_create();
kstep_task_pin(t1, 8, 16);  // CPUs 8-15
kstep_task_pin(t2, 8, 16);  // CPUs 8-15
```
Then run a few ticks to let the tasks settle on node 1. After that, unpin them by expanding their affinity to all CPUs:
```c
kstep_tick_repeat(10);
kstep_task_pin(t1, 1, 16);  // Allow CPUs 1-15 (avoid CPU 0)
kstep_task_pin(t2, 1, 16);  // Allow CPUs 1-15 (avoid CPU 0)
```
Now both tasks are running on node 1 but are eligible for migration to node 0.

**Step 3: Trigger Periodic Load Balance**

Run many ticks to trigger the NUMA-level periodic load balance. The NUMA domain balance interval may be long, so use a generous number of ticks:
```c
kstep_tick_repeat(5000);
```
During these ticks, the load balancer will run on CPUs in node 0 and evaluate whether to pull tasks from node 1.

**Step 4: Detect the Bug**

After the ticks, check which CPU each task is running on:
```c
int cpu1 = task_cpu(t1);
int cpu2 = task_cpu(t2);
bool t1_on_node0 = (cpu1 >= 0 && cpu1 <= 7);
bool t2_on_node0 = (cpu2 >= 0 && cpu2 <= 7);
```

**Expected behavior on the buggy kernel**: The load balancer computes `adjust_numa_imbalance(1, busiest->sum_nr_running=2, busiest->group_weight=8)`. Inside `adjust_numa_imbalance`, `allow_numa_imbalance(2, 8)` evaluates `2 < (8 >> 2) = 2`, which is `false`. The imbalance is NOT suppressed. The function returns `imbalance = 1`. The load balancer proceeds to pull one task from node 1 to node 0. At least one task will have migrated to a CPU on node 0: `t1_on_node0 || t2_on_node0` is true.

**Expected behavior on the fixed kernel**: The load balancer computes `adjust_numa_imbalance(1, local->sum_nr_running + 1 = 0 + 1 = 1, local->group_weight=8)`. Inside `adjust_numa_imbalance`, `allow_numa_imbalance(1, 8)` evaluates `1 < 2`, which is `true`. The imbalance IS allowed. Then `imbalance(1) <= NUMA_IMBALANCE_MIN(2)` is `true`. The function returns 0. The load balancer sets `env->imbalance = 0` and does NOT migrate. Both tasks remain on node 1: `!t1_on_node0 && !t2_on_node0`.

**Step 5: Pass/Fail Criteria**
```c
if (t1_on_node0 || t2_on_node0) {
    kstep_fail("Task migrated to node 0 (cpu1=%d, cpu2=%d) - "
               "NUMA imbalance not properly allowed", cpu1, cpu2);
} else {
    kstep_pass("Both tasks stayed on node 1 (cpu1=%d, cpu2=%d) - "
               "NUMA imbalance correctly allowed", cpu1, cpu2);
}
```

### Potential Complications and Mitigations

1. **NUMA domain balance interval**: The NUMA-level balance interval may be very long. If 5000 ticks is insufficient, increase to 20000 or more. Alternatively, use `kstep_sysctl_write()` to reduce the NUMA balance interval if such a sysctl exists.

2. **Other load balance decisions**: Even with `env->imbalance = 1`, the actual migration requires `find_busiest_queue()` to find a suitable runqueue and `detach_tasks()` to succeed. Cache hotness or other factors could prevent migration even with a non-zero imbalance. To increase reliability, run more tasks (3 instead of 2) to create a larger imbalance signal, but be careful not to exceed the 25% threshold on node 0 after migration.

3. **Driver CPU reservation**: CPU 0 is reserved for the driver. It is on node 0, so it contributes to node 0's `sum_nr_running`. However, since the driver's CPU is typically idle between operations, this should not significantly affect the load balance calculation. If it does, adjust by using 3 tasks on node 1 instead of 2.

4. **Non-determinism**: While the imbalance calculation is deterministic, the actual load balance trigger timing depends on tick alignment with the balance interval. Run the test multiple times if the first attempt is inconclusive.

5. **Kernel-side instrumentation**: For additional debugging, add `printk` statements via a kernel patch or use kprobes/tracepoints to log the arguments passed to `adjust_numa_imbalance()` and the returned value. This can confirm whether the buggy or fixed code path is being taken even if the observable task migration doesn't occur for other reasons.

### Alternative: Testing `find_idlest_group()` (Bug 1)

To test the wakeup placement path, set up 1 task already running on node 0, then wake a second task and observe where it's placed. On the buggy kernel (threshold `sd->span_weight >> 2 = 4`), the second task stays on node 0 because `1 < 4` is true. On the fixed kernel (threshold `group_weight >> 2 = 2`), the check is `(1+1) < 2` which is false, so the task may be placed on node 1 if it has more idle CPUs. This is harder to trigger reliably because it requires the slow wakeup path in `select_task_rq_fair()`, which is not always taken (the fast `wake_affine` path may short-circuit it).
