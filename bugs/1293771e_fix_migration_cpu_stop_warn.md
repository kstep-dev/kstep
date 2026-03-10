# sched: Fix migration_cpu_stop() WARN

- **Commit:** 1293771e4353c148d5f6908fb32d1c1cfd653e47
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

Users were hitting a spurious WARN condition in migration_cpu_stop() when a task was not on the expected runqueue. The warning checked if the task was allowed on a CPU using cpu_of(rq), but rq in that context was the migration stopper CPU's runqueue, not the task's actual runqueue, leading to an incorrect CPU value being tested.

## Root Cause

The code used is_cpu_allowed(p, cpu_of(rq)) to validate task CPU affinity, but cpu_of(rq) returns the CPU of the migration stopper's runqueue, not the task's actual CPU. The earlier branch already established that task_rq(p) != rq, meaning rq does not contain the task, so using cpu_of(rq) produces the wrong CPU for validation and triggers a false positive WARN.

## Fix Summary

Replace is_cpu_allowed(p, cpu_of(rq)) with cpumask_test_cpu(task_cpu(p), &p->cpus_mask) at two locations. This directly checks if the task's actual CPU (obtained via task_cpu(p)) is in the task's allowed CPU affinity mask, providing the correct validation without false warnings.

## Triggering Conditions

The bug is triggered when task migration races with CPU affinity changes in migration_cpu_stop(). Specifically:
- A task needs migration and migration_cpu_stop() is invoked on a stopper CPU
- The task gets migrated to a different runqueue between scheduling the stopper and its execution
- task_rq(p) != rq (task not on stopper's runqueue), taking the second branch
- dest_cpu < 0 (migrate_enable case) and !pending (no migration pending)
- The WARN checks is_cpu_allowed(p, cpu_of(rq)) using stopper's CPU instead of task's CPU
- If task's actual CPU differs from stopper CPU and has different affinity, false WARN triggers
- Race window is larger on non-PREEMPT kernels due to migrate_enable() timing

## Reproduce Strategy (kSTEP)

Use 3+ CPUs with tasks having restricted CPU affinity to create migration/affinity conflicts:
- Setup: Create 2 tasks with different CPU affinity restrictions using kstep_task_create() + kstep_task_pin()
- Force migration scenario: Pin task A to CPU 1-2, task B to CPU 2-3, trigger load imbalance
- Use kstep_tick_repeat() to let tasks run and accumulate load on specific CPUs  
- Change task A's affinity to exclude current CPU using kstep_task_pin(task_a, 2, 3)
- Trigger immediate tick to force migration_cpu_stop() execution with kstep_tick()
- Use on_tick_begin callback to log task runqueue locations and CPU assignments
- Monitor for WARN patterns by checking task_rq() vs migration stopper runqueue mismatches
- Detect bug via spurious warnings when task's actual CPU differs from stopper CPU
- Verify fix by ensuring no false warnings occur after affinity changes
