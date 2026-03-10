# Check cpus_mask, not cpus_ptr in __set_cpus_allowed_ptr(), to fix mask corruption

- **Commit:** fd844ba9ae59b51e34e77105d79f8eca780b3bd6
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When a task is migrate-disabled and `__set_cpus_allowed_ptr()` is called with a new mask equal to the single CPU the task is currently running on, the mask update is lost. The function incorrectly compares against the transitory CPU mask (`cpus_ptr`, which reflects the current CPU during migrate-disable) instead of the long-term affinity mask (`cpus_mask`), causing an early return that skips the actual mask update.

## Root Cause

The bug occurs because the function checks `p->cpus_ptr` (the transitory mask while migrate-disabled) instead of `&p->cpus_mask` (the actual long-term affinity mask). When a task is migrate-disabled, `cpus_ptr` temporarily points to the current CPU only, while `cpus_mask` holds the real target mask. If the new mask matches the current CPU, the equality check against `cpus_ptr` incorrectly triggers, causing the function to return early without updating `cpus_mask`.

## Fix Summary

The fix changes the comparison from checking `p->cpus_ptr` to checking `&p->cpus_mask`, ensuring the function compares against the actual long-term affinity mask rather than the transitory one. This allows the mask update to proceed correctly even when a task is migrate-disabled.

## Triggering Conditions

The bug requires a task to be migrate-disabled when `__set_cpus_allowed_ptr()` is called. During migrate-disable, `p->cpus_ptr` points to a temporary mask containing only the current CPU, while `p->cpus_mask` holds the actual long-term affinity. The bug triggers when:

- A task is migrate-disabled (e.g., holding a per-CPU resource or in RT context)
- `__set_cpus_allowed_ptr()` is called with a new affinity mask
- The new mask equals the single CPU the task is currently running on
- The function incorrectly compares against `p->cpus_ptr` (current CPU only) instead of `&p->cpus_mask`
- This causes early return without updating the actual `cpus_mask`, losing the affinity change

The core scheduler path involves `sched_setaffinity()` → `__set_cpus_allowed_ptr()` while the task has migration disabled.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). In `setup()`, create a task with `kstep_task_create()` and pin it to CPU 1. In `run()`:

- Create a migrate-disabled context by using `kstep_freeze_task()` or manipulating per-CPU resources
- While the task is migrate-disabled, call a function that triggers `__set_cpus_allowed_ptr()` with CPU 1 (current CPU)
- Use direct kernel manipulation to set task affinity via `do_set_cpus_allowed()` or through cgroup CPU affinity changes
- Before the affinity call, log the initial `cpus_mask` value  
- After the call, check if `p->cpus_mask` was updated correctly versus `p->cpus_ptr`
- Use `on_tick_begin()` callback to monitor task migration state and mask values
- Log both `cpus_mask` and `cpus_ptr` to detect when the mask update is lost
- Bug is triggered if `cpus_mask` remains unchanged while `cpus_ptr` shows the current CPU
