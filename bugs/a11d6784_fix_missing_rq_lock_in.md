# sched_ext: Fix missing rq lock in scx_bpf_cpuperf_set()

- **Commit:** a11d6784d7316a6c77ca9f14fb1a698ebbb3c1fb
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

The `scx_bpf_cpuperf_set()` BPF function modifies the runqueue's cpuperf_target field and calls `cpufreq_update_util()` without properly acquiring the rq lock. This unsafe behavior triggers lockdep warnings during scheduler initialization when BPF scheduler programs call this function. The missing lock protection violates locking invariants that these operations depend on.

## Root Cause

The original code accessed the runqueue structure and performed operations that require lock protection without ensuring the rq lock was held. It used only `rcu_read_lock_sched_notrace()` as a workaround, which is insufficient for modifying rq state. The function had no mechanism to properly acquire the required rq lock or detect when operating with mismatched lock contexts.

## Fix Summary

The fix adds proper rq lock handling by checking if an rq lock is already held via `scx_locked_rq()`. If no lock is held, it acquires the rq lock before updating cpuperf_target. If a lock is held for a different CPU, it returns an error to prevent ABBA deadlocks. The lock is properly released after the operation.

## Triggering Conditions

This bug is triggered when BPF scheduler programs call `scx_bpf_cpuperf_set()` during sched_ext initialization without holding the appropriate rq lock. The conditions include:
- sched_ext subsystem being enabled with a BPF scheduler program
- BPF program calling `scx_bpf_cpuperf_set()` in initialization callbacks (like `.init`)  
- Target CPU's runqueue lock not being held by the calling context
- Lockdep being enabled to detect the missing lock protection
- Function modifying `rq->scx.cpuperf_target` field and invoking `cpufreq_update_util()`
- Race window where runqueue state is accessed without serialization

## Reproduce Strategy (kSTEP)

Since this is a sched_ext/BPF-specific locking issue that occurs during scheduler initialization, reproducing it with kSTEP would be challenging as kSTEP operates within the existing scheduler framework rather than loading custom BPF schedulers. A theoretical approach:
- Enable sched_ext support in kernel configuration
- Create a minimal BPF program that calls `scx_bpf_cpuperf_set()` in its `.init` callback
- Use `kstep_sysctl_write()` to enable lockdep checking for scheduler locks
- Load the BPF scheduler via appropriate syscalls in `setup()`
- Monitor kernel logs with `on_tick_begin()` callback for lockdep warnings
- Check for "WARNING: ... at kernel/sched/sched.h:1512 scx_bpf_cpuperf_set" pattern
- Trigger would be detected through kernel warning messages rather than observable scheduling behavior
