# sched/core: Fix a missed update of user_cpus_ptr

- **Commit:** df14b7f9efcda35e59bb6f50351aac25c50f6e24
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

When sched_setaffinity() is called with a CPU mask identical to the task's current cpus_mask, the user_cpus_ptr is not updated. This violates the contract introduced in commit 8f9ea86fdf99 that user_cpus_ptr should always be preserved with the user-requested CPU affinity mask. The bug causes subsequent operations that rely on user_cpus_ptr to reference stale or incorrect user mask information.

## Root Cause

In `__set_cpus_allowed_ptr_locked()`, when the new cpumask equals the current cpus_mask, the code takes an early exit via `goto out` to optimize for the case where no actual migration is needed. However, this optimization failed to account for the requirement to always update user_cpus_ptr when the SCA_USER flag is set, leaving the user mask unsynced even though the affinity call succeeded.

## Fix Summary

The fix adds a conditional check inside the early exit path: if the SCA_USER flag is set (indicating a user-requested affinity change), it now swaps the user_cpus_ptr with the provided user mask before returning. This ensures user_cpus_ptr is always kept in sync with the user's requested affinity, even when no actual task migration occurs.

## Triggering Conditions

- The bug requires calling `sched_setaffinity()` with the SCA_USER flag set
- The provided CPU mask must be identical to the task's current `cpus_mask`
- The task must already have a `user_cpus_ptr` allocated from a previous affinity call
- Any subsequent code that reads `user_cpus_ptr` will observe the stale/old mask instead of the latest user request
- The race occurs in the early exit path of `__set_cpus_allowed_ptr_locked()` before the user mask swap
- No actual task migration or CPU assignment changes occur, making the bug subtle to detect

## Reproduce Strategy (kSTEP)

- **CPUs needed:** Minimum 2 (CPU 0 reserved for driver, test on CPU 1+)
- **Setup:** Create a task with `kstep_task_create()` and pin it to specific CPUs initially
- **Step 1:** Call `kstep_task_pin()` to set initial affinity (triggers user_cpus_ptr allocation)
- **Step 2:** Call `kstep_task_pin()` again with the same CPU mask (reproduces the bug)
- **Detection:** Use a custom callback to inspect the task's `user_cpus_ptr` field directly
- **Verification:** Create a third affinity call with a different mask and check if user_cpus_ptr reflects the second (identical) call or still shows the first one
- **Observable behavior:** On buggy kernels, user_cpus_ptr remains unchanged after step 2
