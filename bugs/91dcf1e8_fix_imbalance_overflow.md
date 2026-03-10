# sched/fair: Fix imbalance overflow

- **Commit:** 91dcf1e8068e9a8823e419a7a34ff4341275fb70
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS

## Bug Description

When the local group is fully busy but its average load is above the system average load, the imbalance calculation in `calculate_imbalance()` can overflow, leading to incorrect load balancing decisions. This causes the local group to be incorrectly selected as a target for pulling load when it should not be, resulting in poor scheduling decisions.

## Root Cause

The `calculate_imbalance()` function was missing a check to validate that the local group's average load is below the system average load before proceeding to calculate the imbalance. Without this check, when `local->avg_load >= sds->avg_load`, the subsequent imbalance computation (involving multiplication and division) could overflow or produce incorrect results, leading to wrong load balancing behavior.

## Fix Summary

Added an early check in `calculate_imbalance()` to return with `env->imbalance = 0` if the local group's average load is greater than or equal to the system average load. This prevents the subsequent imbalance calculation from being executed in this invalid condition, avoiding the overflow and ensuring correct load balancing behavior.

## Triggering Conditions

This bug occurs during load balancing when:
- The local sched group is fully busy (`local->group_type < group_overloaded`)
- Local group's average load is greater than or equal to system average load (`local->avg_load >= sds->avg_load`)
- A busiest group exists that triggers load balancing from the perspective of the local group
- The imbalance calculation in `calculate_imbalance()` proceeds without the missing check
- System topology has multiple scheduling groups (e.g., multiple CPUs/clusters)
- Load distribution is uneven enough to trigger active load balancing

## Reproduce Strategy (kSTEP)

Configure a multi-CPU setup (at least 3 CPUs beyond CPU 0 reserved for driver):
1. **Setup**: Use `kstep_topo_init()` and `kstep_topo_set_cls()` to create multiple scheduling groups
2. **Task creation**: Create several tasks with `kstep_task_create()` and vary their weights/priorities
3. **Load imbalance**: Use `kstep_task_pin()` to create heavy load on some CPUs in the local group, making local avg_load > system avg_load
4. **Trigger balancing**: Use `kstep_tick_repeat()` to advance scheduler ticks and trigger periodic load balancing
5. **Monitor**: Add `on_sched_balance_selected` callback to track load balancing decisions
6. **Detection**: Check if `env->imbalance` overflows or produces incorrect values when local group incorrectly becomes a pull target
7. **Verification**: Compare scheduling decisions on buggy vs fixed kernels using load balance trace outputs
