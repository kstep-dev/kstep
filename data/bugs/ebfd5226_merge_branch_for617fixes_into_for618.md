# sched_ext: Merge branch 'for-6.17-fixes' into for-6.18

- **Commit:** ebfd5226ec365d0901d3ddee4aba9c737137645c
- **Affected file(s):** kernel/sched/core.c, kernel/sched/ext.c, kernel/sched/ext_idle.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

When checking if a task is migration-disabled within BPF sched_ext callbacks, the code cannot simply check `p->migration_disabled` because migration is always disabled for the current task while running BPF code (due to the prolog/epilog wrapping). This causes the current task to incorrectly appear as migration-disabled even if it wasn't disabled before entering the callback, leading to incorrect scheduling decisions in BPF-based schedulers.

## Root Cause

The BPF entry code (`__bpf_prog_enter`) and exit code (`__bpf_prog_exit`) disable and re-enable migration respectively, making the current task always appear migration-disabled while inside a sched_ext callback. Directly checking `p->migration_disabled == 1` cannot distinguish between tasks that are actually migration-disabled versus the current task that appears disabled only due to BPF entry.

## Fix Summary

Introduces `is_bpf_migration_disabled()` helper function that correctly determines migration-disabled status in BPF context by checking whether a task is the current task when `migration_disabled == 1`. Also updates helper functions to accept a `struct scx_sched *sch` parameter for better error reporting context.

## Triggering Conditions

This bug occurs in sched_ext BPF callbacks (specifically ops.enqueue() or other non-ops.select_cpu() callbacks) when:
- A BPF scheduler calls scx_bpf_select_cpu_dfl() or scx_bpf_select_cpu_and() kfuncs
- The target task is the current task (the one executing the BPF callback)
- The current task has migration_disabled == 1 due to BPF entry (__bpf_prog_enter())
- The scheduler incorrectly treats the current task as migration-disabled when it's not actually disabled
- This causes idle CPU selection to fail (-EBUSY) even when idle CPUs are available in the task's allowed cpumask

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs. Create a sched_ext scheduler that calls scx_bpf_select_cpu_and() from ops.enqueue():
1. Setup: Enable sched_ext with a custom BPF scheduler implementing ops.enqueue() callback
2. Create task with kstep_task_create() and enable it on CPU 1 with kstep_task_pin(task, 1, 1)
3. In ops.enqueue() callback, call scx_bpf_select_cpu_and() for the current task
4. Observe that scx_bpf_select_cpu_and() returns -EBUSY even with idle CPUs available
5. Use kSTEP callbacks on_sched_softirq_begin() to trace when the bug occurs during enqueue operations
6. Check that task->migration_disabled == 1 but task == current (indicating false positive)
7. Log the return value of scx_bpf_select_cpu_and() - should be -EBUSY on buggy kernel, valid CPU on fixed kernel
8. Verify fix by running same test on fixed kernel where scx_bpf_select_cpu_and() returns valid idle CPUs
