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
