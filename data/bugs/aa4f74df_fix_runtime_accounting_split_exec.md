# sched: Fix runtime accounting w/ split exec & sched contexts

- **Commit:** aa4f74dfd42ba4399f785fb9c460a11bd1756f0a
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

When proxy-exec is enabled, the scheduler has two "current" contexts: the scheduler context (rq->donor) and the execution context (rq->curr). The bug is that runtime accounting was not correctly separated between these two contexts when they differ. Specifically, sum_exec_runtime and cgroup accounting were being charged to the wrong task, leading to incorrect task accounting visible to userland and incorrect cgroup statistics.

## Root Cause

The original code's `update_curr_se()` function charged all accounting (sum_exec_runtime, cgroup accounting, etc.) against a single task without considering that with proxy-exec, the donor task and the running task may differ. It did not distinguish that the running task's sum_exec_runtime should reflect that the rq->curr task is actually executing, while cgroup and vruntime accounting should be charged to the donor task from which the time is being "donated".

## Fix Summary

The fix refactors `update_curr_se()` to `update_se()` and adds logic to properly separate accounting between the two contexts. When the entity is a task, sum_exec_runtime is now charged to the actual running task (rq->curr), while cgroup accounting is charged to the donor task (rq->donor). Vruntime and other scheduler accounting remains against the donor task as intended.

## Triggering Conditions

This bug manifests when proxy-exec is enabled and there are two distinct "current" contexts:
- The scheduler context (rq->donor) - the task from which time is being donated  
- The execution context (rq->curr) - the task actually executing on the CPU
- Both contexts must refer to different tasks (rq->donor != rq->curr)
- Runtime accounting occurs via `update_curr()` in kernel/sched/fair.c during scheduling events
- The bug causes sum_exec_runtime and cgroup accounting to be charged to the donor task instead of the running task
- This results in incorrect task accounting visible to userspace and wrong cgroup statistics

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Create a scenario where proxy-exec causes split contexts:
- In `setup()`: Enable proxy-exec support and create tasks in different cgroups using `kstep_cgroup_create()` and `kstep_cgroup_add_task()`
- Create 2 tasks: one donor task and one proxy task using `kstep_task_create()`  
- Configure different cgroup weights via `kstep_cgroup_set_weight()` to make accounting differences observable
- In `run()`: Use `kstep_task_pin()` to place tasks on CPU 1, then trigger proxy-exec via lock contention or priority inheritance
- Use `kstep_tick_repeat()` to advance scheduling and accumulate runtime accounting
- Monitor via `on_tick_begin()` callback to log task sum_exec_runtime values and cgroup stats
- Bug detected when: donor task's sum_exec_runtime increases despite proxy task actually running
- Verify fix: after patch, proxy task's sum_exec_runtime should increase while donor's cgroup gets charged
