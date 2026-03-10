# sched_ext: Fix scx_bpf_dsq_insert() backward binary compatibility

- **Commit:** 2dbbdeda77a61b39dc4a34dfce873907cfea2c4b
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

Old BPF binaries that were compiled with the original `scx_bpf_dsq_insert()` function became unable to resolve the symbol after a prior commit renamed it to `scx_bpf_dsq_insert___compat` and introduced a new bool-returning version with the original name. This caused a symbol resolution failure at runtime, breaking backward binary compatibility with existing BPF programs.

## Root Cause

The previous commit (cded46d97159) attempted to use a `___compat` suffix for the old void-returning interface, relying on libbpf to ignore the suffix when matching symbols. While libbpf ignores `___suffix` on the BPF side, it does not do so for kernel-side symbols. Old binaries looking for `scx_bpf_dsq_insert` in the kernel could not find `scx_bpf_dsq_insert___compat`, causing symbol resolution to fail.

## Fix Summary

The fix reverses the naming scheme: the original `scx_bpf_dsq_insert()` is restored as the old void-returning interface (maintaining backward compatibility), while the new bool-returning version is renamed to `scx_bpf_dsq_insert___v2`. This allows old binaries to continue resolving the original symbol while new code can use the v2 variant.
