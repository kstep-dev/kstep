# sched/fair: Fix sched_avg fold

- **Commit:** 6ab7973f254071faf20fe5fcc502a3fe9ca14a47
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

A regression was introduced when the sched_avg update was refactored to be "folded" in commit 089d84203ad4. The weight factor `se_weight()` was missed in two critical locations: `enqueue_load_avg()` and `dequeue_load_avg()` functions. This caused incorrect calculation of the load_sum when enqueueing and dequeueing entities, leading to inaccurate weighted load tracking in the scheduler. The regression was detected by the kernel test robot.

## Root Cause

When the sched_avg update was refactored to consolidate the update logic, the `se_weight()` multiplier was accidentally omitted from the `delta_sum` parameter in both `enqueue_load_avg()` and `dequeue_load_avg()` function calls. The load_sum must be weighted by the entity's weight factor to correctly represent the entity's contribution to the runqueue's load. Without this factor, the load accounting becomes inconsistent with the intended behavior.

## Fix Summary

The fix applies the `se_weight(se)` multiplier to `se->avg.load_sum` (and its negation) in both `enqueue_load_avg()` and `dequeue_load_avg()` functions. This ensures that the weighted load is correctly accumulated when entities are enqueued and dequeued from the runqueue.

## Triggering Conditions

The bug is triggered whenever tasks with different nice values (weights) are enqueued or dequeued from CFS runqueues. The incorrect load accounting affects the scheduler's per-entity load average (PELT) calculations, leading to inaccurate load balancing decisions. The condition occurs in normal scheduler operation when:
- Tasks with non-default nice values (different `se_weight()` factors) wake up or sleep
- The CFS runqueue's `avg.load_sum` accumulates incorrect weighted contributions
- Load balancing logic receives distorted load metrics, causing suboptimal task migration
- The regression manifests as increased scheduling latencies, particularly in workloads with mixed-priority tasks

## Reproduce Strategy (kSTEP)

Set up multiple CPUs (2-4) with tasks having different nice values to trigger weighted load accounting:
- Create tasks with different priorities using `kstep_task_set_prio(task, nice_value)`
- Use `kstep_task_wakeup()` and `kstep_task_pause()` to trigger enqueue/dequeue operations
- Monitor CFS runqueue load metrics in `on_tick_begin()` callback by accessing `cpu_rq(cpu)->cfs.avg`
- Compare `cfs_rq->avg.load_sum` before and after task operations to detect incorrect accumulation
- Log weighted vs unweighted contributions: `se_weight(se) * se->avg.load_sum` vs `se->avg.load_sum`
- Detect the bug by observing load_sum values that don't match expected weighted contributions
- Run a latency-sensitive benchmark (similar to schbench) to observe increased scheduling delays
- The bug manifests as inconsistent load metrics leading to poor load balancing decisions
