# Revert "sched/core: Reduce cost of sched_move_task when config autogroup"

- **Commit:** 76f970ce51c80f625eb6ddbb24e9cb51b977b598
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A performance regression was introduced by an optimization that added an early bailout in sched_move_task() when a task's sched_task_group already matched the target group. This early return prevented detach_task_cfs_rq() from being called for exiting tasks, causing incorrect group utilization calculations and poor load balancing decisions. For short-lived fork/exit tasks (UnixBench spawn test), this resulted in inappropriate CPU selection away from the local CPU, degrading performance by ~30%.

## Root Cause

The optimization compared p->sched_task_group with sched_get_task_group(p) and returned early if they matched, skipping the dequeue/sched_change_group/enqueue sequence. However, this prevented detach_task_cfs_rq() from being called, which is necessary to update sgs->group_util in the load balancing calculations. With incorrect group utilization values (~900 instead of correct accounting), group_is_overloaded() and group_has_capacity() made wrong scheduling decisions, causing WF_FORK wakeups to select non-local CPUs much more frequently.

## Fix Summary

The fix reverts the optimization entirely, removing the early bailout check. This ensures sched_move_task() always performs the full dequeue/sched_change_group/enqueue sequence, properly updating group utilization metrics and restoring correct load balancing behavior for fork/exit task sequences.

## Triggering Conditions

The bug manifests when:
- Autogroup is enabled (CONFIG_SCHED_AUTOGROUP=y) and tasks belong to the same autogroup as their cgroup
- Short-lived tasks execute fork/exit sequences rapidly (similar to UnixBench spawn test)
- During task exit, sched_autogroup_exit_task() calls sched_move_task() with p->sched_task_group == sched_get_task_group(p)
- The early bailout prevents detach_task_cfs_rq() from being called, leaving stale group utilization values
- Subsequent WF_FORK wakeups use incorrect sgs->group_util (~900 vs correct accounting) in load balancing decisions
- This causes group_is_overloaded() and group_has_capacity() to incorrectly classify groups as overloaded/fully_busy
- Results in select_task_rq_fair() choosing non-local CPUs inappropriately for new fork tasks

## Reproduce Strategy (kSTEP)

Configure a multi-CPU topology with autogroup enabled and create rapid fork/exit task sequences:
- Set up 4 CPUs (kSTEP needs CPU 0 reserved) with single-level MC sched domain topology
- Use kstep_topo_init() and kstep_topo_set_mc() to create appropriate scheduler domains  
- Enable autogroup via kernel configuration or sysctl if possible
- In run(), create a parent task that repeatedly forks short-lived children using kstep_task_fork()
- Each child should exit quickly after minimal work (use kstep_task_pause() to simulate exit)
- Monitor load balancing decisions with on_sched_balance_selected() callback
- Track group utilization values and CPU selection during WF_FORK wakeups in the callback
- Log instances where tasks are scheduled away from smp_processor_id() when local CPU has capacity
- Compare CPU selection patterns - bug should show increased non-local placement vs expected behavior
- Measure group_util values in sched_group_capacity to detect stale utilization accounting
