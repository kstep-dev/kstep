# sched/mmcid: Implement deferred mode change

- **Commit:** 9da6ccbcea3de1fa704202e3346fe6c0226bfc18
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** sched/mmcid

## Bug Description

When CPU affinity changes allow a task's MM context to support more CPUs, the scheduler may need to transition the MM CID ownership mode from per-CPU mode back to per-task mode. However, affinity changes occur while the runqueue lock is held, making it impossible to perform the mode change and required fixup immediately. Without deferred handling, this mode transition would be missed or cause deadlocks, leading to incorrect MM CID allocation and potential scheduling inconsistencies.

## Root Cause

Affinity changes are processed under runqueue lock protection in the scheduler's hot path. The function `mm_update_cpus_allowed()` cannot directly perform the mode change and CPU CID fixup work because it would violate locking order (acquiring additional locks like the MM mutex would create deadlock potential with other code paths). The original code had only a placeholder comment indicating this work needed to be deferred but was not implemented.

## Fix Summary

The fix implements a deferred mode change mechanism using irq_work and a workqueue. When an affinity change requires a mode transition, `mm_update_cpus_allowed()` queues an irq_work handler that schedules a workqueue task. The actual mode change and fixup is performed by `mm_cid_work_fn()` when the work runs, properly serialized through the MM's mutex. This allows the expensive fixup to happen outside the runqueue lock while remaining race-safe with concurrent fork() and exit() operations.

## Triggering Conditions

The bug occurs when CPU affinity changes increase the allowed CPU set for tasks sharing a memory context (MM). This triggers when:
- Multiple tasks share the same MM (via clone(CLONE_VM) or threads)
- MM CID is in per-CPU mode (users count >= pcpu_threshold)
- An affinity change via sched_setaffinity() or cpuset expands the allowed CPU set
- The new CPU count causes users >= pcpu_threshold condition to fail, requiring mode switch back to per-task
- The affinity update happens during scheduler hot path under rq->lock
- Without the fix, `mm_update_cpus_allowed()` has only a placeholder comment and defers nothing
- The mode transition gets skipped, leaving MM CID allocation inconsistent between per-CPU and per-task modes

## Reproduce Strategy (kSTEP)

Setup requires 3+ CPUs (CPU 0 reserved for driver). Create multiple tasks sharing MM via `kstep_task_fork()`, trigger per-CPU mode by exceeding threshold, then expand affinity to force mode switch:

```c
// In setup(): Create task group that will share MM
task = kstep_task_create();
kstep_task_fork(task, 8); // Create 8+ tasks sharing MM to exceed pcpu_threshold

// In run(): Pin tasks to small CPU set to trigger per-CPU mode  
kstep_task_pin(task, 1, 2); // Constrain to CPUs 1-2
kstep_tick_repeat(10);      // Let MM CID enter per-CPU mode

// Expand affinity to trigger deferred mode change requirement
kstep_task_pin(task, 1, 4); // Expand to CPUs 1-4
kstep_tick_repeat(5);       // Process affinity change under rq->lock

// Use on_tick_end() callback to log MM CID state and detect inconsistency
// Check if mm->mm_cid.update_deferred flag is set but work not scheduled (bug)
```
