# sched_ext: Avoid NULL scx_root deref in __scx_exit()

- **Commit:** c8fafb34854af4f5036ee0cf582e4b00556c5cd0
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

A sched_ext scheduler can trigger `__scx_exit()` from a BPF timer callback (e.g., in scx_tickless), where the global `scx_root` pointer may not be safely dereferenced. Without proper validation, this causes a kernel NULL pointer dereference crash at address 0x330, as the code attempts to access fields of a potentially invalid or NULL `scx_root` during scheduler teardown.

## Root Cause

The `__scx_exit()` function directly dereferences `scx_root` without checking validity or providing RCU protection. When a BPF timer fires asynchronously during scheduler shutdown, `scx_root` may become NULL or stale. Without RCU synchronization or NULL checks, concurrent access leads to a crash when the function tries to read `scx_root->exit_info`, `scx_root->exit_kind`, or `scx_root->error_irq_work`.

## Fix Summary

The fix wraps the critical section with RCU read-side protection (`rcu_read_lock()`/`rcu_read_unlock()`), safely dereferences `scx_root` using `rcu_dereference()`, and checks if the pointer is valid before proceeding. If `scx_root` is NULL, the function exits early without attempting further dereferences, preventing the crash.

## Triggering Conditions

This bug occurs in the sched_ext subsystem during scheduler teardown when:
- A sched_ext BPF scheduler is active and uses BPF timers (e.g., scx_tickless)
- The scheduler is being disabled/unloaded, causing `scx_root` to become NULL
- A BPF timer callback fires asynchronously during this teardown window
- The timer callback triggers `__scx_exit()` which directly dereferences the now-NULL `scx_root`
- No RCU protection exists around the `scx_root` access, creating a race condition
- The crash occurs at offset 0x330 when accessing `scx_root->exit_info`

## Reproduce Strategy (kSTEP)

*Note: This bug requires sched_ext BPF scheduler support, which kSTEP may not currently provide.*

Theoretical reproduction approach:
- Requires 2+ CPUs (CPU 0 reserved for driver)
- In `setup()`: Enable sched_ext subsystem and load a BPF scheduler with timer callbacks
- Create tasks with `kstep_task_create()` and distribute across CPUs 1-2
- In `run()`: Start BPF timer operations that periodically call functions leading to `__scx_exit()`
- Use `kstep_tick_repeat()` to advance time while timers are active
- Trigger scheduler shutdown/disable while timers are still firing
- Use `on_tick_begin()` callback to monitor for kernel panics or NULL derefs
- Detection: Monitor kernel logs for "BUG: kernel NULL pointer dereference, address: 0000000000000330"
- Success: Kernel crash with RIP pointing to `__scx_exit+0x2b`
