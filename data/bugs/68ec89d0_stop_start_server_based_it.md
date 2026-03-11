# sched/debug: Stop and start server based on if it was active

- **Commit:** 68ec89d0e99156803bdea3c986c0198624e40ea2
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** Deadline (DL server)

## Bug Description

The DL server interface for applying parameters unconditionally calls `dl_server_stop()` and checks CFS-internals (`rq->cfs.h_nr_queued`) to determine server state. This is error-prone because it relies on CFS-specific implementation details to manage the deadline server, which can cause incorrect state management when the server is not actually active or when adding new server types in the future.

## Root Cause

The original code checks `rq->cfs.h_nr_queued` (a CFS-specific field) to determine whether to restart the server, and unconditionally stops the server before applying parameter changes. This couples the deadline server logic to CFS internals and fails to properly verify if the server is actually active before attempting to stop/restart it.

## Fix Summary

The fix introduces proper server state checking using `dl_server_active()`, the same function used by the deadline scheduler itself to determine active status. It only stops the server if it's actually active, and only starts it if it was previously active and runtime parameters remain valid. This decouples the deadline server logic from CFS internals and ensures correct state management.

## Triggering Conditions

The bug triggers when modifying DL server parameters (runtime/period) via sysfs while the server state doesn't match CFS queue state. Specifically:
- DL server is inactive (never started or already stopped) but CFS has queued tasks (`rq->cfs.h_nr_queued > 0`)  
- DL server parameters are changed via `/proc/sys/kernel/sched_domain/cpu*/fair_server_runtime` or `fair_server_period`
- The buggy code unconditionally calls `dl_server_stop()` on inactive server, then incorrectly restarts based on CFS queue state
- This causes invalid server state transitions and potential crashes when the server logic assumes proper start/stop sequencing
- The bug becomes critical when adding new server types that don't correlate with CFS internals

## Reproduce Strategy (kSTEP)

Requires 2+ CPUs (CPU 0 reserved for driver). Create CFS tasks to populate `h_nr_queued` while keeping DL server inactive:
- In `setup()`: Create CFS tasks with `kstep_task_create()`, configure DL server parameters via `kstep_sysctl_write()`
- In `run()`: Use `kstep_task_wakeup()` to queue CFS tasks on CPU 1, ensuring `rq->cfs.h_nr_queued > 0`
- Keep DL server inactive by setting invalid parameters initially (runtime=0 or runtime>period)  
- Trigger bug via `kstep_sysctl_write("kernel.sched_domain.cpu1.fair_server_runtime", "1000000")`
- Use `on_tick_begin()` callback to log server state transitions and CFS queue status
- Detect bug by monitoring inconsistent server state: `dl_server_stop()` called on inactive server
- Check for kernel warnings/crashes from invalid state transitions in DL server logic
