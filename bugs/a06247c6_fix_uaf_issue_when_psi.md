# psi: Fix uaf issue when psi trigger is destroyed while being polled

- **Commit:** a06247c6804f1a7c86a2e5398a4c1f1db1471848
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** core

## Bug Description

A use-after-free vulnerability occurs when a psi trigger is destroyed while actively being polled. When a write operation on a psi file replaces an existing trigger with a new one, the old trigger's waitqueue is freed. However, a pending poll() operation may still access the freed trigger->event_wait, causing a crash or undefined behavior.

## Root Cause

The original code used reference counting (kref) combined with RCU to manage trigger lifetimes, but the `psi_trigger_replace()` function could destroy a trigger while it was actively being accessed by poll operations. The refcount mechanism was complex and vulnerable to races where a trigger could be freed while poll operations held stale references to it.

## Fix Summary

The fix disallows redefining an existing psi trigger by returning EBUSY if a write operation is attempted on a file descriptor that already has a trigger. Additionally, it simplifies the synchronization mechanism by removing reference counting and using acquire/release semantics (smp_load_acquire/smp_store_release) instead of RCU + kref, eliminating the race condition entirely.

## Triggering Conditions

The bug requires a race between two threads: one writing to a PSI file descriptor that already has a trigger (causing `psi_trigger_replace()` to free the old trigger's waitqueue), and another thread polling the same file descriptor. The sequence: Thread A opens /proc/pressure/memory, writes a trigger, Thread B starts polling that fd, Thread A writes a new trigger (triggering `psi_trigger_replace()` which destroys the first trigger and its `event_wait` waitqueue), Thread B's poll operation accesses the freed `trigger->event_wait`, causing use-after-free.

## Reproduce Strategy (kSTEP)

Since PSI triggers are userspace file operations on /proc/pressure files, this bug cannot be directly reproduced through kSTEP's kernel-level task scheduling APIs. The bug occurs in userspace-to-kernel interaction via file operations, not in core scheduler paths. A reproduction would require simulating userspace file I/O operations (open, write, poll) on PSI files with concurrent trigger replacement, which is outside kSTEP's current task/scheduling simulation scope. Consider extending kSTEP with PSI file operation simulation or test this bug through userspace tools that can perform concurrent PSI file operations.
