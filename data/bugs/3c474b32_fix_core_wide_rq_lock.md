# sched: Fix Core-wide rq->lock for uninitialized CPUs

- **Commit:** 3c474b3239f12fe0b00d7e82481f36a1f31e79ab
- **Affected file(s):** kernel/sched/core.c, kernel/sched/sched.h
- **Subsystem:** core scheduling

## Bug Description

When code iterates through all possible CPUs (via `for_each_possible_cpu()`) and calls `rq_lock()` on uninitialized CPUs (CPUs that have not yet been onlined), the locking logic fails because `rq->core` is initialized to `NULL`. This causes dereferencing a null pointer when the lock mechanism tries to access the core's lock structure, breaking the core-wide locking guarantees.

## Root Cause

The original code initialized `rq->core = NULL` in `sched_init()` with the assumption that `rq->core` would always be set up before any locking operation. However, when iterating through all possible CPUs (including those that may never be onlined or are waiting to be initialized), code that accesses `rq->core` before it has been properly initialized fails. This is a race between CPU initialization logic and code paths that operate on all possible CPUs.

## Fix Summary

The fix initializes each `rq->core` to point to itself (`rq->core = rq`), making each CPU initially its own core leader. This ensures the lock mechanism has a valid target even for uninitialized CPUs. Corresponding CPU hotplug hooks (`sched_core_cpu_deactivate()` and `sched_core_cpu_dying()`) are added to properly manage core transitions when CPUs come online or go offline, maintaining correct core-wide locking semantics throughout the CPU lifecycle.

## Triggering Conditions

The bug is triggered when code paths iterate through all possible CPUs using `for_each_possible_cpu()` and attempt to call `rq_lock()` on CPUs that have not yet been brought online. This creates a null pointer dereference when the locking logic tries to access `rq->core` which was initialized to `NULL` in `sched_init()`. The race occurs specifically in the core scheduling subsystem where the core-wide locking mechanism requires valid `rq->core` pointers. The triggering path involves the `raw_spin_rq_lock()` function attempting to access `rq->core` before CPU hotplug initialization has properly set up the core relationships for SMT siblings.

## Reproduce Strategy (kSTEP)

Requires at least 2 CPUs (CPU 0 reserved for driver). Create a driver that explicitly iterates through possible CPUs and attempts core-wide locking operations before proper initialization:

1. Setup: Use `kstep_topo_init()` and `kstep_topo_set_smt()` to define SMT topology with pairs like ["0", "1-2", "1-2"]
2. Run: Create tasks with `kstep_task_create()` and pin them using `kstep_task_pin()` to different CPU sets
3. Trigger: Use direct kernel access to iterate `for_each_possible_cpu()` and call `rq_lock()` on each rq
4. Detection: Add `on_tick_begin()` callback to monitor for kernel panics or null pointer dereferences
5. Verification: Check kernel logs with `TRACE_INFO()` for access to uninitialized `rq->core` fields
6. Alternative: Use `kstep_tick_repeat()` with tasks distributed across SMT siblings to stress the locking paths
