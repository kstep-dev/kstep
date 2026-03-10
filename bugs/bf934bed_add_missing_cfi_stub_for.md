# sched_ext: Add missing cfi stub for ops.tick

- **Commit:** bf934bed5e2fd81f767d75c05fb95f0333a1b183
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The CFI (Control Flow Integrity) stub for the `ops.tick` callback was missing from the BPF-based scheduler infrastructure. Without this stub, the scheduler would fail to load after pending BPF changes that introduce the `tick` callback support. The missing stub breaks the validation and initialization of the scheduler operations structure.

## Root Cause

When adding support for the `ops.tick` callback in the sched_ext BPF scheduler framework, the corresponding CFI stub function and its registration in the `__bpf_ops_sched_ext_ops` structure were inadvertently omitted. The BPF validation logic requires stubs for all callback operations to be present in the operations table, and the absence of the `tick` stub caused initialization to fail.

## Fix Summary

The fix adds two missing pieces: the `tick_stub` function definition and its registration in the `__bpf_ops_sched_ext_ops` structure. This allows the scheduler to properly initialize when the `tick` callback is present in BPF-based scheduler implementations.

## Triggering Conditions

This bug occurs during BPF scheduler loading when the sched_ext framework validates callback stubs. Specifically:
- The sched_ext framework must be enabled (CONFIG_SCHED_CLASS_EXT=y)
- A BPF scheduler program implements the `ops.tick` callback
- The scheduler loading process validates that all implemented callbacks have corresponding CFI stubs in `__bpf_ops_sched_ext_ops`
- Without the `tick_stub` function and its registration, BPF validation fails during scheduler attachment
- The bug manifests as scheduler loading failure, not runtime scheduling behavior
- Timing/race conditions are not relevant - this is a deterministic validation failure

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce with kSTEP as it occurs during BPF scheduler loading, not runtime behavior:
- **CPUs needed**: 2+ (CPU 0 reserved for driver)
- **Setup limitation**: kSTEP lacks direct BPF scheduler loading APIs
- **Alternative approach**: Monitor for scheduler loading failures via kernel logs
- **Detection method**: Check for BPF validation errors when tick callback is present
- **kSTEP operations**: Use `kstep_tick_repeat()` to trigger tick events and log any missing stub errors
- **Callbacks**: Use `on_tick_begin()` to intercept tick processing
- **Validation**: Look for kernel error messages indicating missing CFI stub validation
- **Note**: This bug requires BPF scheduler infrastructure that may not be directly accessible through kSTEP's current API surface
