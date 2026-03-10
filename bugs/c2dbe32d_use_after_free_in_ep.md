# sched/psi: Fix use-after-free in ep_remove_wait_queue()

- **Commit:** c2dbe32d5db5c4ead121cf86dabd5ab691fb47fe
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

When a non-root cgroup is deleted while a thread is polling on a pressure file within that cgroup, the polling waitqueue is freed via `psi_trigger_destroy()`. However, the polling thread still holds a reference to the pressure file and attempts to access the freed waitqueue when the file is closed or the thread exits, causing a use-after-free crash. This can be triggered in production by removing a cgroup with active pollers.

## Root Cause

The lifetime of the PSI trigger's waitqueue is not tied to the file's actual lifetime. When `cgroup_file_release()` is called during cgroup deletion, it destroys the waitqueue via `psi_trigger_destroy()`. However, the polling thread may still have the file open and calls `remove_wait_queue()` on the freed waitqueue during `ep_free()`, resulting in a use-after-free violation.

## Fix Summary

Changed `wake_up_interruptible(&t->event_wait)` to `wake_up_pollfree(&t->event_wait)` in `psi_trigger_destroy()`. The `wake_up_pollfree()` function is designed specifically for cases where the waitqueue lifetime is not tied to the file's lifetime, preventing the waitqueue from being accessed after destruction.

## Triggering Conditions

- **PSI subsystem**: Requires a non-root cgroup with pressure monitoring enabled
- **Task state**: A thread must be actively polling on a pressure file (memory, cpu, or io) using epoll
- **Cgroup deletion timing**: The cgroup must be deleted while the polling thread still holds a reference to the pressure file
- **Race condition**: Thread exit/file close must occur after `psi_trigger_destroy()` frees the waitqueue but before `ep_free()` completes
- **Memory management**: Use-after-free occurs in `remove_wait_queue()` when accessing the freed waitqueue's spinlock
- **Call path sequence**: `cgroup_rmdir` → `psi_trigger_destroy` (frees waitqueue) → later `ep_free` → `remove_wait_queue` (accesses freed memory)

## Reproduce Strategy (kSTEP)

- **CPUs needed**: Minimum 2 CPUs (CPU 0 reserved for driver, use CPUs 1-2 for tasks)
- **Setup**: Create a non-root cgroup using `kstep_cgroup_create("test_cgroup")`, enable pressure monitoring via `kstep_cgroup_write("test_cgroup", "memory.pressure", "some 100000 1000000")`
- **Task creation**: Create a userspace task that opens and polls the pressure file using `kstep_write("/proc/pressure/memory", ...)` to simulate epoll polling
- **Race trigger sequence**: Use `kstep_tick()` to advance time, then simulate cgroup deletion by calling filesystem operations that trigger `psi_trigger_destroy()`
- **Detection method**: Monitor for use-after-free using callbacks like `on_tick_end()` to check if waitqueue access occurs after destruction
- **Observation**: Check kernel logs for KASAN use-after-free reports or memory corruption in `remove_wait_queue()` during task exit
- **Timing control**: Use `kstep_sleep()` and careful tick timing to ensure the polling thread outlives the cgroup deletion
