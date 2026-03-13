# PELT: Runnable Average Initialization Causes False Overload Classification

**Commit:** `e21cf43406a190adfcc4bfe592768066fb3aaa9b`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.8-rc3
**Buggy since:** v5.7-rc1 (introduced by commit `070f5e860ee2` "sched/fair: Take into account runnable_avg to classify group")

## Bug Description

When a new task is forked, the scheduler initializes its Per-Entity Load Tracking (PELT) signals in `post_init_entity_util_avg()`. The `util_avg` is carefully estimated based on the current CFS runqueue utilization and CPU capacity, yielding a reasonable initial value that reflects half the remaining capacity. However, the `runnable_avg` signal was unconditionally set to `cpu_scale` — the full CPU capacity (typically 1024) — regardless of the actual system state or the task's expected behavior.

This inflated `runnable_avg` value is problematic because commit `070f5e860ee2` introduced a new `group_runnable` metric into the load balancer's group classification logic. The `group_runnable` field in `struct sg_lb_stats` aggregates `cfs_rq->avg.runnable_avg` across all CPUs in a scheduling group, and this metric is used in both `group_has_capacity()` and `group_is_overloaded()` to determine whether a scheduling group has spare capacity or is overloaded. When newly forked tasks each contribute the maximum `runnable_avg` value, the aggregate `group_runnable` becomes artificially inflated, causing the group to be classified as `group_overloaded` even when the actual CPU utilization is low.

The consequence is a performance regression in fork-heavy workloads such as the `reaim` benchmark. When a scheduling group is incorrectly classified as overloaded, the load balancer becomes less aggressive at pulling tasks to that group during load balancing, leading to poor task distribution and reduced throughput. The kernel test robot reported this regression, which was traced back to the overly aggressive initial `runnable_avg` value.

## Root Cause

The root cause lies in the `post_init_entity_util_avg()` function in `kernel/sched/fair.c`. When a new CFS task is created, this function runs after the initial scheduling entity setup to provide reasonable initial PELT signal estimates. Prior to the fix, the code was:

```c
sa->runnable_avg = cpu_scale;
```

This sets the scheduling entity's `runnable_avg` to the full CPU capacity scale value (typically 1024 for a full-capacity CPU). The `runnable_avg` signal represents the fraction of time a task is runnable (either running or waiting to run on the runqueue). Setting it to the maximum implies the task has been continuously runnable for its entire (estimated) existence, which is a gross overestimate — especially for short-lived tasks that may run briefly and exit.

In contrast, `util_avg` (which represents actual CPU utilization) was carefully initialized using a formula that considers existing CFS runqueue load: `sa->util_avg = cfs_rq->avg.util_avg * se->load.weight / (cfs_rq->avg.load_avg + 1)`, capped at half the remaining CPU capacity. This yields a reasonable estimate. But `runnable_avg` ignored all of this and always used the maximum.

When `attach_entity_cfs_rq()` runs (called at the end of `post_init_entity_util_avg()`), the task's `runnable_avg` is added to the CFS runqueue's aggregate: `cfs_rq->avg.runnable_avg += se->avg.runnable_avg`. This means each newly forked task adds up to 1024 to the per-CPU `runnable_avg`. In `update_sg_lb_stats()`, these per-CPU values are summed into `sgs->group_runnable += cpu_runnable(rq)`, which returns `cfs_rq_runnable_avg(&rq->cfs)`.

The inflated `group_runnable` then causes `group_is_overloaded()` to return `true` via the check: `(sgs->group_capacity * imbalance_pct) < (sgs->group_runnable * 100)`. Similarly, `group_has_capacity()` returns `false` via the same runnable check. This misclassification propagates through `group_classify()` which returns `group_overloaded` instead of a more benign type like `group_has_spare`.

When a scheduling group is classified as `group_overloaded`, the load balancer in `find_busiest_group()` treats it differently — it expects heavy contention and reduces the aggressiveness of task migration. This means tasks are not pulled to underutilized groups as quickly as they should be, leading to imbalanced CPU utilization and throughput degradation.

## Consequence

The primary observable impact is a significant performance regression in fork-heavy benchmarks, specifically the `reaim` benchmark. The kernel test robot at Intel reported measurable throughput drops after the introduction of commit `070f5e860ee2`. Workloads that frequently fork short-lived tasks are disproportionately affected because each fork inflates the runnable average with a maximum value, and short-lived tasks exit before the PELT decay can bring the signal back to realistic levels.

The false `group_overloaded` classification affects the load balancer's `find_busiest_group()` and `calculate_imbalance()` functions. When a group that is not truly overloaded is classified as such, the balancer may select the wrong busiest group or compute incorrect migration amounts. Tasks that should be migrated to balance the system are not moved, leading to CPUs sitting idle while other CPUs are overloaded. In the mailing list discussion, Valentin Schneider noted that an earlier version of this initialization was avoided because hackbench performed slightly worse, but the subsequent `reaim` regression was deemed more significant.

Additionally, Holger Hoffstätte reported a secondary issue where `loadavg` values became stuck at pre-suspend levels after resume from suspend-to-RAM. While this was reported in the same thread, it appears to be a separate issue. The core performance impact remains: incorrect group classification leads to suboptimal task placement across CPUs, measurable in fork-intensive multi-threaded workloads.

## Fix Summary

The fix is a single-line change in `post_init_entity_util_avg()`:

```c
- sa->runnable_avg = cpu_scale;
+ sa->runnable_avg = sa->util_avg;
```

Instead of initializing `runnable_avg` to the full CPU capacity, it is set equal to the already-computed `util_avg`. This reflects the reality that a newly forked task has no waiting time yet — it has not been competing for CPU time with other tasks, so its runnable time should be equal to its utilized time. The `util_avg` value was already computed earlier in the same function using a formula that accounts for the current CFS runqueue load and remaining CPU capacity, making it a much more accurate initial estimate.

This change ensures that the per-entity `runnable_avg` starts at a reasonable level that won't artificially inflate the aggregate `cfs_rq->avg.runnable_avg`. As the task runs and the PELT signals decay and accumulate naturally, the `runnable_avg` will diverge from `util_avg` if the task experiences queuing delays (i.e., if it is runnable but not running because other tasks are using the CPU). But initially, assuming zero queuing delay is both correct and conservative.

The fix is minimal and correct because it preserves the existing initialization logic for `util_avg` and simply reuses that computed value. It does not change the PELT decay mechanics, the group classification logic, or any other part of the load balancer. The root cause was purely an initialization issue — the signals themselves work correctly once initialized to reasonable values.

## Triggering Conditions

The bug requires the following conditions to manifest:

- **CONFIG_SMP=y**: The `post_init_entity_util_avg()` function's SMP implementation (with the runnable_avg initialization) is only compiled when SMP is enabled. The `!CONFIG_SMP` version is a no-op.
- **CONFIG_FAIR_GROUP_SCHED**: While not strictly required for the initialization bug, the load balancing group classification that observes the inflated signal benefits from task group scheduling being enabled.
- **Multiple CPUs**: At least 2 CPUs are needed so that load balancing across scheduling groups is triggered. More CPUs (4+) make the problem more visible because there are more scheduling groups to misclassify.
- **Fork-heavy workload**: The bug is triggered when multiple tasks are forked in rapid succession. Each fork adds a maximum `runnable_avg` contribution (1024) to the CPU's aggregate. If tasks fork faster than PELT can decay the signals, `group_runnable` stays artificially inflated.
- **Short-lived tasks**: The bug's impact is amplified when forked tasks are short-lived. If tasks run for a long time, their `runnable_avg` would naturally converge to realistic values through PELT decay. But short tasks contribute the maximum initial value and exit before decay corrects the overestimate.
- **Timing**: The effect is most pronounced during bursts of fork activity. A steady state of long-running tasks would see PELT signals converge, but rapid fork/exit cycles keep the signal inflated.

The probability of triggering the bug is high in fork-intensive workloads. The `reaim` benchmark, which exercises process creation and destruction, is a known trigger. Any workload pattern involving repeated `fork()`/`exec()`/`exit()` cycles (such as shell script execution, make -j builds, or web server request handling via forking) would be affected to varying degrees.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why can this bug not be reproduced with kSTEP?

**The kernel version is too old.** The bug was introduced in v5.7-rc1 (commit `070f5e860ee2`) and fixed in v5.8-rc3 (commit `e21cf43406a1`). kSTEP supports Linux v5.15 and newer only. The bug does not exist in any kernel version that kSTEP supports — it was already fixed more than a year before v5.15 was released. Checking out the buggy commit (`e21cf43406a1~1`) would produce a kernel from the v5.8-rc2 era, which is outside kSTEP's supported range.

### 2. What would need to be added to kSTEP to support this?

Even if version support were extended, reproducing this bug would require:

- **Task forking**: kSTEP has `kstep_task_fork()` which could be used to trigger the `post_init_entity_util_avg()` code path. The forked task would get the inflated `runnable_avg`.
- **PELT signal observation**: kSTEP would need to be able to read `se->avg.runnable_avg` and `cfs_rq->avg.runnable_avg` from within the driver. This could be done via `KSYM_IMPORT` and direct structure access through `internal.h`.
- **Load balancer group classification observation**: To detect the actual impact (incorrect `group_overloaded` classification), the driver would need to observe load balancing decisions. kSTEP provides `on_sched_balance_begin` and `on_sched_balance_selected` callbacks, which could be used to monitor the group types being selected.
- **Multiple rapid forks**: The driver would need to fork multiple short-lived tasks quickly to inflate `group_runnable` before PELT decay corrects the signals.

The kSTEP framework itself has the necessary primitives (task creation, forking, PELT signal reading), so from a capability perspective the bug is reproducible. The sole blocker is the kernel version constraint.

### 3. Version constraint

The fix targets v5.8-rc3, which is before kSTEP's minimum supported version of v5.15. This is the definitive reason this bug goes to `drivers_unplanned`.

### 4. Alternative reproduction methods

To reproduce this bug outside kSTEP:

- **Direct kernel testing**: Build a v5.7 or v5.8-rc1/rc2 kernel with `CONFIG_SMP=y`. Run the `reaim` benchmark and compare throughput against the same kernel with commit `e21cf43406a1` cherry-picked. The performance regression should be measurable as reduced throughput in the fork-heavy test scenarios.
- **PELT tracing**: Use `ftrace` or `trace_sched_stat_*` tracepoints to observe the `runnable_avg` values of newly forked tasks. On the buggy kernel, freshly forked tasks will show `runnable_avg == 1024` (cpu_scale). On the fixed kernel, they will show `runnable_avg == util_avg` (a much smaller value).
- **Load balancer tracing**: Use the `sched:sched_load_balance` tracepoint family or add `printk` instrumentation to `group_classify()` to observe that newly forked tasks cause scheduling groups to be classified as `group_overloaded` when they should be `group_has_spare` or `group_fully_busy`.
- **Synthetic workload**: Write a simple program that forks many short-lived children in a loop (e.g., `fork(); exit(0);` in a tight loop) and measure task distribution across CPUs using `/proc/stat` or `mpstat`. On the buggy kernel, task distribution will be more uneven because the load balancer is less aggressive at rebalancing.
