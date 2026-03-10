# sched/core: Fix priority checking for DL server picks

- **Commit:** 4b26cfdd395638918e370f62bd2c33e6f63ffb5e
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core scheduling, deadline server

## Bug Description

In core scheduling, CFS tasks served by a deadline (DL) server were not being given the correct priority when competing with RT tasks. This caused CFS tasks to be completely starved despite being boosted by the DL server mechanism. The DL server's priority boosting was completely ignored, leading to tasks that should have deadline-level priority being scheduled at fair-class priority instead.

## Root Cause

The priority comparison functions `__task_prio()` and `prio_less()` did not account for the `dl_server` field. When a CFS task is served by a DL server, it should have deadline priority, but the code was not checking for this case. Additionally, when comparing deadlines, the comparison was using the task's own `dl` entity instead of the server's deadline entity when `dl_server` was present.

## Fix Summary

The fix adds checks for `dl_server` in two places: (1) in `__task_prio()`, tasks with a DL server now return priority -1 (deadline), giving them higher priority than RT tasks; (2) in `prio_less()`, the deadline comparison logic now uses the DL server's deadline entity when available instead of the task's own deadline entity, ensuring correct priority ordering.

## Triggering Conditions

This bug manifests in core scheduling environments where:
- Core scheduling is enabled (`CONFIG_SCHED_CORE=y`)
- CFS tasks are being served by a deadline server (`task->dl_server != NULL`)
- These DL-server-boosted CFS tasks compete with RT tasks on the same CPU or core
- Priority comparison occurs in `__task_prio()` and `prio_less()` functions during scheduling decisions
- Without the fix, DL-server-boosted CFS tasks get fair class priority (120) instead of deadline priority (-1)
- RT tasks with priority 0-99 incorrectly preempt the DL-server-boosted CFS tasks
- Results in complete CFS starvation despite DL server bandwidth guarantees

## Reproduce Strategy (kSTEP)

**Setup**: Requires at least 2 CPUs, with CPU topology that enables core scheduling interactions.

**Steps**:
1. In `setup()`: Create one CFS task and one RT task using `kstep_task_create()`
2. Configure RT task with high priority: `kstep_task_set_prio(rt_task, 10)` 
3. Enable DL server for CFS task (may require direct manipulation of `task->dl_server` field)
4. Pin both tasks to same CPU range: `kstep_task_pin(task, 1, 2)`
5. In `run()`: Wake both tasks with `kstep_task_wakeup()`, then `kstep_tick_repeat(100)`
6. Use `on_tick_begin()` callback to monitor current task: `kstep_output_curr_task()`
7. **Detection**: Check if CFS task gets CPU time when DL server should boost its priority above RT
8. On buggy kernel: RT task completely starves CFS task; on fixed kernel: CFS task runs with DL priority
