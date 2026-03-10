# sched/mmcid: Prevent pointless work in mm_update_cpus_allowed()

- **Commit:** 0d032a43ebeb9bf255cd7e3dad5f7a6371571648
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (MM CID)

## Bug Description

mm_update_cpus_allowed() is unnecessarily invoked during migrate_disable() and migrate_enable() operations, causing the mm_allowed mask to be updated based on temporary CPU restrictions. When a task is temporarily restricted via migrate_disable() to a CPU it's already allowed on, the mm_allowed mask should not be modified, as this is a temporary state that will be restored on migrate_enable(). Calling mm_update_cpus_allowed() during these operations can cause the mm_allowed mask to be incorrectly updated based on temporary restrictions.

## Root Cause

The mm_update_cpus_allowed() function was being called unconditionally in __do_set_cpus_allowed() for all affinity changes, including those marked with SCA_MIGRATE_ENABLE or SCA_MIGRATE_DISABLE flags. In these cases, the cpus_mask is not being permanently changed (migrate_disable restricts to an already-allowed CPU, and migrate_enable restores the actual mask), so calling mm_update_cpus_allowed() is pointless and can cause the mm_allowed mask to be inflated with temporary restrictions.

## Fix Summary

The fix moves the mm_update_cpus_allowed() call from __do_set_cpus_allowed() to set_cpus_allowed_common(), placing it after the early return for SCA_MIGRATE_ENABLE and SCA_MIGRATE_DISABLE cases. This ensures the function is only called for actual affinity changes, not for temporary migrate_disable/migrate_enable operations.

## Triggering Conditions

The bug is triggered when migrate_disable() or migrate_enable() operations are performed on tasks. These operations invoke the scheduler affinity management code path through __do_set_cpus_allowed() with SCA_MIGRATE_DISABLE or SCA_MIGRATE_ENABLE flags. The key conditions are:
- A task that is allowed to run on multiple CPUs (cpus_mask has multiple bits set)
- migrate_disable() is called to temporarily restrict the task to its current CPU
- The current CPU must be one the task was already allowed on (no actual affinity change)
- mm_update_cpus_allowed() gets called unnecessarily, inflating mm_allowed mask with temporary restrictions
- migrate_enable() restores the original mask but may trigger additional unnecessary mm updates

## Reproduce Strategy (kSTEP)

Use at least 2 CPUs (CPU 1-2, with CPU 0 reserved for driver). Create a task with multi-CPU affinity and trigger migrate operations:
- In setup(): Create task with kstep_task_create(), allow it to run on CPUs 1-2 with kstep_task_pin(task, 1, 2)
- In run(): Place task on CPU 1, then simulate migrate_disable() via targeted CPU restriction to current CPU
- Use internal scheduler hooks or direct kernel calls to trigger SCA_MIGRATE_DISABLE path in __do_set_cpus_allowed()
- Monitor mm_update_cpus_allowed() invocations through custom logging in on_tick_begin() callback
- Detect bug by counting unnecessary mm_update_cpus_allowed() calls during temporary restrictions
- Compare call counts before/after migrate operations to identify pointless invocations
- Log mm_allowed mask state changes to verify incorrect updates during temporary restrictions
