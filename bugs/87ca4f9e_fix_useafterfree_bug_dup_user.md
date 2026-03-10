# Fix use-after-free bug in dup_user_cpus_ptr()

- **Commit:** 87ca4f9efbd7cc649ff43b87970888f2812945b8
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A use-after-free bug occurs in `dup_user_cpus_ptr()` when a task is being forked while another CPU affinity update is happening concurrently. When `sched_setaffinity()` clears `user_cpus_ptr` under `pi_lock` protection, the forking task's `dup_user_cpus_ptr()` may read and dereference a freed pointer without holding any lock, leading to use-after-free and potentially double-free errors. This race condition was reintroduced to affect all architectures after commit 851a723e45d1.

## Root Cause

The function `dup_user_cpus_ptr()` accesses `src->user_cpus_ptr` without any synchronization mechanism, while concurrent execution of `do_set_cpus_allowed()` can clear (free) this pointer under `pi_lock`. The original code checked `user_cpus_ptr` without holding `pi_lock`, then allocated and copied the mask outside the lock, creating a window where the source task's pointer could be freed between the check and the copy operation.

## Fix Summary

The fix ensures `dst->user_cpus_ptr` is always cleared upfront, performs the check of `src->user_cpus_ptr` under `pi_lock` protection, and uses a temporary `user_mask` variable to safely handle the allocation and assignment. If `src->user_cpus_ptr` is reset to NULL between the initial check and lock acquisition, the code gracefully handles this by freeing the allocated temporary mask and returning 0.

## Triggering Conditions

The bug requires a race between `dup_user_cpus_ptr()` during fork/clone and `do_set_cpus_allowed()` during CPU affinity updates. The source task must have a non-NULL `user_cpus_ptr` (created by prior `sched_setaffinity()` call). The timing window occurs when `dup_user_cpus_ptr()` reads `src->user_cpus_ptr` without `pi_lock`, allocates memory, then attempts to copy the mask while `do_set_cpus_allowed()` concurrently clears and frees `user_cpus_ptr` under `pi_lock`. This creates use-after-free when the forking task dereferences the freed pointer, and potential double-free if both tasks attempt to free the same memory.

## Reproduce Strategy (kSTEP)

Create two tasks: a parent that will be forked and an affinity-setter task. Use `kstep_task_create()` to create both tasks, then call `sched_setaffinity()` on the parent to allocate `user_cpus_ptr`. Create multiple kthreads using `kstep_kthread_create()` - one repeatedly calls `fork()` while another repeatedly calls `sched_setaffinity()` to clear the parent's affinity. Use `kstep_tick_repeat()` in a loop to create timing opportunities for the race. Monitor for kernel panics, use-after-free detection (if KASAN enabled), or memory corruption indicators. The race window is narrow, so repeat the fork/setaffinity sequence hundreds of times across multiple ticks to increase trigger probability.
