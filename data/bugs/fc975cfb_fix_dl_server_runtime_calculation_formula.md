# sched/deadline: Fix dl_server runtime calculation formula

- **Commit:** fc975cfb36393db1db517fbbe366e550bcdcff14
- **Affected file(s):** kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

On Big.LITTLE systems, the fair deadline server's runtime was being incorrectly scaled by both frequency and capacity scale-invariance factors, causing RT tasks to be blocked from running on LITTLE cpus for multiple seconds—far exceeding the configured 50ms per second runtime. In the example provided, a 50ms runtime was scaled down to ~238μs, creating a 209:1 ratio that allowed the fair server to monopolize a LITTLE cpu for over 10 seconds before exhausting its supposed 50ms runtime.

## Root Cause

The `update_curr_dl_se()` and `dl_server_update_idle_time()` functions were unconditionally applying frequency and capacity scaling to all deadline entities via `dl_scaled_delta_exec()`. This scaling is appropriate for regular deadline tasks but incorrect for the fair server, which must use fixed (unscaled) time to prevent unpredictable delays to real-time tasks. The scaling was making the fair server's runtime accounting appear much smaller than actual elapsed time on lower-capacity CPUs.

## Fix Summary

The fix conditionally applies scaling only to non-server deadline entities. For the fair server specifically, it now uses the raw `delta_exec` value without any scaling transformation. This ensures the fair server's 50ms per second runtime is enforced based on actual elapsed time, preventing it from starving RT tasks on smaller CPUs.

## Triggering Conditions

The bug requires a Big.LITTLE CPU topology with mixed capacity/frequency scales. The fair deadline server must be active with CFS tasks consuming its runtime, while RT tasks exist on lower-capacity CPUs. The bug manifests when `update_curr_dl_se()` processes the fair server's runtime accounting on a LITTLE CPU, where both frequency and capacity scaling factors are significantly below SCHED_CAPACITY_SCALE. This causes the server's actual runtime consumption to be drastically underaccounted, allowing it to monopolize the CPU far beyond its configured 50ms budget and block RT tasks from running.

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved for driver). Set up Big.LITTLE topology using `kstep_topo_init()`, `kstep_topo_set_mc()`, and `kstep_topo_apply()`. Configure asymmetric capacities via `kstep_cpu_set_capacity()` with CPU 1 at full capacity (1024) and CPU 2 at reduced capacity (~400-500). Create CFS tasks with `kstep_task_create()` and pin them to the LITTLE CPU using `kstep_task_pin()`. Create RT tasks and monitor their scheduling behavior. Use `on_tick_begin()` callback to track fair server runtime consumption and RT task delays. The bug triggers when fair server runtime depletes much slower than expected on LITTLE CPUs, causing RT tasks to experience multi-second delays exceeding the 50ms/1s deadline server budget.
