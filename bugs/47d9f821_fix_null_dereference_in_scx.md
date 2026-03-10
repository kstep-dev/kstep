# sched_ext: Fix NULL dereference in scx_bpf_cpu_rq() warning

- **Commit:** 47d9f8212826753c482df8189d18ca212eb5ae73
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The `scx_bpf_cpu_rq()` kfunc dereferences `scx_root` without checking if it is NULL, causing a kernel NULL pointer dereference crash when called before a BPF scheduler is fully attached. This occurs when the kfunc is invoked from BPF timers or during `ops.init()`, resulting in a kernel panic with a crash at the deprecation warning code.

## Root Cause

The bug occurs because `scx_root` is accessed directly without RCU protection or a NULL check. When the scheduler is not yet fully registered, `scx_root` is NULL, and the code immediately dereferences it with `sch->warned_deprecated_rq`, causing the crash. The original code did not use RCU synchronization to safely read this potentially-NULL global pointer.

## Fix Summary

The fix wraps the `scx_root` access with RCU read-side critical sections (`rcu_read_lock()`/`rcu_read_unlock()`) and uses `rcu_dereference()` to safely read the pointer. It also adds a NULL check (`if (likely(sch) && ...)`) before dereferencing `sch`, ensuring the deprecation warning is only printed once the scheduler is fully registered.

## Triggering Conditions

The bug is triggered in the sched_ext subsystem when `scx_bpf_cpu_rq()` kfunc is invoked before a BPF scheduler is fully attached and `scx_root` is NULL. Specific conditions include:
- sched_ext must be compiled in but not have a fully registered BPF scheduler
- The `scx_bpf_cpu_rq()` kfunc must be called during the attachment window (e.g., from BPF timers, ops.init(), or early BPF program execution)
- The code path reaches the deprecation warning logic where `sch->warned_deprecated_rq` is accessed
- No RCU synchronization protects the `scx_root` access, making the race window between attachment and kfunc invocation critical
- The crash occurs at offset 0x331 in the struct, corresponding to the `warned_deprecated_rq` field access

## Reproduce Strategy (kSTEP)

**Note:** This bug requires sched_ext support which is not currently available in the kSTEP framework. A full reproduction would require extending kSTEP with sched_ext capabilities.

**Hypothetical approach if sched_ext support was added:**
- Setup: 2+ CPUs needed (CPU 0 reserved for driver)
- Use `kstep_scx_load_scheduler()` to begin BPF scheduler attachment without completing it
- During attachment, trigger `kstep_scx_call_kfunc("scx_bpf_cpu_rq", cpu_id)` from a timer callback
- Monitor for NULL pointer dereference crash using kernel log callbacks
- Observe crash at specific RIP offset (scx_bpf_cpu_rq+0x30) with address 0x331
- Verify fix by ensuring the kfunc returns safely when `scx_root` is NULL after applying the patch
