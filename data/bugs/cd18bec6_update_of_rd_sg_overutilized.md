# Fix update of rd->sg_overutilized

- **Commit:** cd18bec668bb6221a54f03d0b645b7aed841f825
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (load balancing, load balance statistics)

## Bug Description

The `set_rd_overutilized()` function in the root domain load balancing path was being called with the wrong status flag (`sg_overloaded` instead of `sg_overutilized`). This causes the root domain's over-utilization flag to be incorrectly set based on the overloaded status rather than the actual over-utilization status, leading to incorrect load balancing decisions and suboptimal task placement across CPUs.

## Root Cause

A copy-paste error introduced in commit 4475cd8bfd9b when separate `->overloaded` and `->overutilized` flags were created. The code at line 10664 in `update_sd_lb_stats()` incorrectly passes `sg_overloaded` to `set_rd_overutilized()` instead of using `sg_overutilized`. While the else-if branch below correctly uses `sg_overutilized`, the initial root domain case does not.

## Fix Summary

Change the parameter passed to `set_rd_overutilized()` from `sg_overloaded` to `sg_overutilized` in the root domain update path. This ensures the root domain's over-utilization status is tracked correctly based on the actual over-utilization metric rather than the overload metric.

## Triggering Conditions

This bug manifests during load balancing when `update_sd_lb_stats()` processes the root domain (when `env->sd->parent == NULL`). The conditions needed are:
- Load balancing must occur at the root sched_domain level (no parent domain)  
- Some sched_groups must be overloaded (`sg_overloaded = true`) while others are over-utilized (`sg_overutilized = true`)
- The root domain's `rd->sg_overutilized` flag gets incorrectly set to the overloaded status
- This causes subsequent load balancing decisions to use wrong over-utilization information
- Most visible when CPU utilization and load metrics diverge (e.g., different capacity CPUs, RT/DL tasks)

## Reproduce Strategy (kSTEP)

Create asymmetric CPU topology with 4+ CPUs to trigger root domain load balancing. Setup tasks that create overload without over-utilization:
1. Use `kstep_topo_init()`, `kstep_topo_set_cls()` with different CPU clusters, and `kstep_topo_apply()`
2. Set different CPU capacities with `kstep_cpu_set_capacity()` (some full, some reduced)
3. Create multiple CFS tasks with `kstep_task_create()` and pin them with `kstep_task_pin()`
4. Configure tasks to create overloaded but not over-utilized state (many low-util tasks on high-capacity CPUs)
5. Use `on_sched_balance_begin()` callback to inspect `rd->sg_overutilized` flag during load balancing
6. Log the `sg_overloaded` and `sg_overutilized` values computed in `update_sd_lb_stats()`
7. Verify mismatch: `rd->sg_overutilized` incorrectly follows `sg_overloaded` instead of `sg_overutilized`
