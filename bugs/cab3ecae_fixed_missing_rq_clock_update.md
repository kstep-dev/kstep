# Fixed missing rq clock update before calling set_rq_offline()

- **Commit:** cab3ecaed5cdcc9c36a96874b4c45056a46ece45
- **Affected file(s):** kernel/sched/core.c, kernel/sched/topology.c
- **Subsystem:** core, topology

## Bug Description

When using a cpufreq governor that hooks into the scheduler via cpufreq_add_update_util_hook(), a missing rq clock update warning can be triggered during CPU hotplug operations. The warning occurs because cpufreq_update_util() calls back into the scheduler expecting the runqueue clock to be up-to-date, but the clock update tracking was missing when set_rq_offline() was called through rq_attach_root().

## Root Cause

The update_rq_clock() call was only present in one specific caller (sched_cpu_deactivate()) of set_rq_offline(), not within set_rq_offline() itself. When set_rq_offline() is called from rq_attach_root() during CPU hotplug, the clock is never updated before the function executes, leaving the clock tracking in an incorrect state when cpufreq_update_util() is eventually invoked through the rt scheduling class chain.

## Fix Summary

The fix moves update_rq_clock() from sched_cpu_deactivate() into set_rq_offline() so that all callers are protected. Additionally, rq_attach_root() is changed to use rq_lock_irqsave() instead of raw_spin_rq_lock_irqsave() to properly manage runqueue clock flags according to the scheduler's locking conventions.

## Triggering Conditions

The bug requires a cpufreq governor that registers a utilization update hook via cpufreq_add_update_util_hook(). The trigger occurs during CPU hotplug operations when rq_attach_root() calls set_rq_offline() without first updating the runqueue clock. The specific path involves the RT scheduling class: rq_attach_root() → set_rq_offline() → rq_offline_rt() → __disable_runtime() → sched_rt_rq_enqueue() → enqueue_top_rt_rq() → cpufreq_update_util(), where cpufreq_update_util() calls data->func(data, rq_clock(rq), flags) expecting an up-to-date clock. The bug manifests as a "missing update_rq_clock()" warning when the runqueue clock tracking is stale due to the missing update_rq_clock() call in set_rq_offline().

## Reproduce Strategy (kSTEP)

Use 2+ CPUs with RT tasks and trigger CPU hotplug-like operations that invoke rq_attach_root(). Create RT tasks with kstep_task_create() + kstep_task_fifo() and pin them to specific CPUs with kstep_task_pin(). Use kstep_topo_init(), modify CPU topology with kstep_topo_set_cls() or kstep_topo_set_pkg(), then apply changes with kstep_topo_apply() to trigger root domain reattachment. Alternatively, use cgroup operations like kstep_cgroup_create() + kstep_cgroup_set_cpuset() to change CPU sets, forcing rq_attach_root() calls. Monitor kernel logs via on_tick_begin() or on_sched_softirq_end() callbacks for "missing update_rq_clock()" warnings. The bug reproduces when set_rq_offline() is called through rq_attach_root() while RT tasks are active and the rq clock is stale, triggering the cpufreq utilization update path without proper clock tracking.
