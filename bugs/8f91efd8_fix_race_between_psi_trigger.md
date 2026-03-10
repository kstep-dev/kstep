# psi: Fix race between psi_trigger_create/destroy

- **Commit:** 8f91efd870ea5d8bc10b0fcc9740db51cd4c0c83
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

A race condition exists between `psi_trigger_destroy()` and `psi_trigger_create()` that can cause a kernel panic. When one thread is destroying a PSI trigger while another is creating one, the `poll_timer` and `poll_wait` structures can be reinitialized while `del_timer_sync()` is attempting to access them, leading to invalid memory access and corruption of the timer and wait queue data structures.

## Root Cause

The `poll_timer` and `poll_wait` are initialized in `psi_trigger_create()` without proper synchronization against their destruction. Although `trigger_lock` and RCU protect the `poll_task` pointer, they do not protect these timer and wait queue structures. A window exists where `psi_trigger_destroy()` has cleared `poll_task` but has not yet called `del_timer_sync()`, during which `psi_trigger_create()` can reinitialize the timer, causing the subsequent `del_timer_sync()` call to operate on an invalid or reinitialized structure.

## Fix Summary

The fix moves the initialization of `poll_wait` and `poll_timer` from `psi_trigger_create()` to `group_init()`, ensuring they are initialized only once when the PSI group is created rather than being reinitialized on each trigger creation. Additionally, `del_timer()` (non-sync) is moved inside the lock in `psi_trigger_destroy()` before releasing the lock, eliminating the race window entirely.

## Triggering Conditions

The race occurs in the PSI (Pressure Stall Information) subsystem when:
- Multiple threads concurrently call `psi_trigger_create()` and `psi_trigger_destroy()` 
- First trigger destroy: thread clears `poll_task` pointer, releases `trigger_lock`, then calls `synchronize_rcu()` followed by `del_timer_sync()`
- During RCU grace period: second thread creates new trigger, reinitializes `poll_timer` with `timer_setup()`
- Race window: `del_timer_sync()` attempts to synchronize with reinitialized timer, causing corruption
- Panic occurs when accessing invalid `poll_wait->wait_queue_entry` or `poll_timer->entry->next` structures
- Requires PSI enabled (`psi=1` or default) and userspace creating/destroying PSI triggers via `/proc/pressure/*`

## Reproduce Strategy (kSTEP)

Reproducing this race requires triggering concurrent PSI operations, which is challenging within kSTEP's kernel-space constraints:
- **CPUs needed**: 2+ (CPU 0 reserved, need concurrent threads on CPU 1+)  
- **Setup**: Create multiple kthread workers with `kstep_kthread_create()`, enable PSI if disabled via `kstep_sysctl_write()`
- **Trigger creation**: Use kernel's internal PSI APIs or create cgroups with `kstep_cgroup_create()` to indirectly trigger PSI initialization
- **Race simulation**: `kstep_kthread_syncwake()` to synchronize threads, then attempt concurrent PSI trigger create/destroy operations
- **Detection**: Use `on_tick_begin()` callback to monitor PSI group state, check for timer corruption or wait queue corruption
- **Alternative**: Create high memory/IO pressure with multiple tasks to trigger PSI naturally, then rapidly create/destroy cgroups to race PSI trigger operations
- **Validation**: Monitor kernel logs for panics, or check PSI group timer/wait structures for corruption patterns
