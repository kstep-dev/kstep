# sched_ext: Fix use of uninitialized variable in scx_bpf_cpuperf_set()

- **Commit:** f4fa7c25f632cd925352b4d46f245653a23b1d1a
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

The `scx_bpf_cpuperf_set()` BPF kfunc attempts to dereference an uninitialized local variable `sch` instead of the intended global `scx_root` pointer. This causes undefined behavior when the function tries to access the scheduler structure, potentially resulting in a crash, memory corruption, or data read from an arbitrary memory location.

## Root Cause

A typo in the code uses `rcu_dereference(sch)` where `sch` is a local variable that was never initialized. The intended reference was to the global `scx_root` pointer, which contains the actual scheduler structure needed to set CPU performance levels.

## Fix Summary

The fix corrects the typo by changing `rcu_dereference(sch)` to `rcu_dereference(scx_root)`, ensuring that the function dereferences the correct global variable containing the scheduler structure.

## Triggering Conditions

This bug is triggered when:
- sched_ext is enabled and a BPF scheduler is loaded and active
- A BPF program invokes the `scx_bpf_cpuperf_set()` kfunc to modify CPU performance levels
- The function attempts to dereference the uninitialized local `sch` variable instead of `scx_root`
- The uninitialized pointer contains garbage data, leading to undefined behavior when accessing scheduler state
- This occurs in the CPU performance scaling code path within the sched_ext framework
- Any subsequent operations on the dereferenced garbage pointer can cause memory corruption, crashes, or unpredictable behavior

## Reproduce Strategy (kSTEP)

Note: This bug requires sched_ext support which may not be available in kSTEP's current framework. If sched_ext is not supported, this reproduction strategy serves as a theoretical approach:

- **CPUs needed**: At least 2 CPUs (CPU 0 reserved for driver, CPU 1+ for testing)
- **Setup**: Enable sched_ext if available, load a minimal BPF scheduler that calls `scx_bpf_cpuperf_set()`
- **In setup()**: Create test tasks with `kstep_task_create()`, configure basic CPU topology with `kstep_topo_init()`
- **In run()**: Wake tasks with `kstep_task_wakeup()`, trigger performance scaling operations that would call the buggy kfunc
- **Detection**: Use memory debugging tools or add instrumentation to detect access to uninitialized memory
- **Observation**: Monitor for kernel crashes, memory corruption, or unexpected behavior when performance scaling is requested
- **Verification**: Compare behavior between buggy and fixed kernels to confirm the undefined behavior is resolved
