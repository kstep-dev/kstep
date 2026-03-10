# sched/fair: Fix condition of avg_load calculation

- **Commit:** 6c8116c914b65be5e4d6f66d69c8142eb0648c22
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The `update_sg_wakeup_stats()` function was calculating average load (`avg_load`) for scheduler groups in the wrong conditions. The code was computing `avg_load` whenever the group type was less than `group_fully_busy`, but the accompanying comment correctly stated that this value only makes sense when the group is fully busy or overloaded. This mismatch caused incorrect average load calculations for groups in states where this metric is meaningless, potentially leading to suboptimal load balancing decisions.

## Root Cause

The condition `if (sgs->group_type < group_fully_busy)` was inverted compared to the intended logic. The code was calculating `avg_load` for all group types that precede `group_fully_busy` in the enumeration, when it should only calculate for the specific states `group_fully_busy` and `group_overloaded`. This logic error contradicted both the explanatory comment and the correct behavior observed in other functions using `avg_load`.

## Fix Summary

The fix changes the condition from `sgs->group_type < group_fully_busy` to `sgs->group_type == group_fully_busy || sgs->group_type == group_overloaded`, ensuring that average load is only calculated when the group is in one of the two states where the metric is meaningful. This aligns the code with its intent and the rest of the codebase.

## Triggering Conditions

- **Scheduler subsystem**: CFS load balancing during task wakeup in `find_idlest_group()` path
- **Code path**: `update_sg_wakeup_stats()` called from `find_idlest_group()` during task placement
- **Required state**: Scheduler groups with `group_has_spare` type (groups with available capacity)
- **System topology**: Multi-CPU system with scheduling domains/groups (NUMA nodes, CPU clusters, or sockets)
- **Load conditions**: Groups with spare capacity where tasks could potentially be placed
- **Impact**: Incorrect `avg_load` calculations for under-utilized groups leading to suboptimal task placement decisions
- **Observable effect**: Task wakeup latency increases and load balancing becomes less effective due to wrong metrics

## Reproduce Strategy (kSTEP)

- **CPUs needed**: At least 4 CPUs (CPU 0 reserved for driver, CPUs 1-4 for topology)
- **Setup**: Create multi-cluster topology with `kstep_topo_set_cls()` to establish scheduling groups
- **Initial state**: Create scheduler groups where some have spare capacity (`group_has_spare` type)
- **Trigger sequence**:
  1. `kstep_task_create()` multiple tasks and place them unevenly across clusters
  2. Use `kstep_task_pin()` to create load imbalance with some groups under-utilized
  3. `kstep_task_wakeup()` a new task to trigger `find_idlest_group()` path
- **Detection method**: Hook `on_sched_balance_begin()` to capture group statistics during load balancing
- **Bug indication**: Log shows `avg_load` being calculated for `group_has_spare` groups (should be zero/unset)
- **Validation**: Compare `avg_load` values between buggy and fixed kernels for groups with spare capacity
