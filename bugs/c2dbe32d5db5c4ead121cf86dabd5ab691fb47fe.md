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
