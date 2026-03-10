# Revert "sched_ext: Use rhashtable_lookup() instead of rhashtable_lookup_fast()"

- **Commit:** df10932ad740ba1f871b6dd2ddafc7dc8cea944f
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

A previous commit changed `find_user_dsq()` to use `rhashtable_lookup_fast()` instead of `rhashtable_lookup()`, which triggered a suspicious RCU usage warning. The warning indicated improper use of `rcu_dereference_check()` when calling `rhashtable_lookup_fast()` while holding a spinlock (the runqueue lock). This API choice violated RCU locking requirements and could lead to runtime safety issues.

## Root Cause

The `rhashtable_lookup_fast()` function is designed to be called without holding RCU read-side locks, as it assumes the caller does not have explicit synchronization preventing table deletion. However, `find_user_dsq()` is called from within scheduler code that holds `rq->__lock` (the runqueue lock), creating an RCU semantics violation. The `rhashtable_lookup()` variant should be used when the caller holds a lock that prevents table deletion, which is the case here.

## Fix Summary

The fix reverts the function call back to `rhashtable_lookup()`, which is the correct API choice for contexts where a spinlock (rather than RCU read-side locking) prevents table deletion. This restores proper RCU usage semantics and eliminates the lockdep warning.

## Triggering Conditions

The bug is triggered when `find_user_dsq()` is called during sched_ext task enqueuing while holding the runqueue spinlock (`rq->__lock`). This happens specifically in the `find_dsq_for_dispatch() → do_enqueue_task() → enqueue_task_scx()` path during task wakeup (`ttwu_do_activate() → sched_ttwu_pending()`). The `rhashtable_lookup_fast()` function performs RCU dereference checks that expect RCU read-side locking, but the caller holds a spinlock instead. This generates lockdep warnings about suspicious RCU usage when CONFIG_PROVE_RCU is enabled, typically during IRQ context or SMP cross-calls when tasks are being enqueued.

## Reproduce Strategy (kSTEP)

To reproduce this bug, create a sched_ext BPF scheduler that uses user-defined dispatch queues and trigger task wakeups that force DSQ lookups. Use at least 2 CPUs and enable CONFIG_PROVE_RCU. In `setup()`, load a simple sched_ext BPF program that defines multiple user DSQs via `scx_bpf_create_dsq()`. Create multiple tasks with `kstep_task_create()` and assign them to different CPUs with `kstep_task_pin()`. In `run()`, trigger cross-CPU wakeups using `kstep_task_pause()` followed by `kstep_task_wakeup()` while tasks are distributed across CPUs, forcing `find_user_dsq()` calls during the enqueue path. Use `on_tick_begin()` callback to log task states. The RCU warning should appear in dmesg when `rhashtable_lookup_fast()` is called while holding `rq->__lock` during the `ttwu_do_activate()` path.
