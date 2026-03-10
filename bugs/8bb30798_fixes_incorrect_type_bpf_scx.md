# sched_ext: Fixes incorrect type in bpf_scx_init()

- **Commit:** 8bb30798fd6ee79e4041a32ca85b9f70345d8671
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT

## Bug Description

The function `bpf_scx_init()` attempts to check if a BTF type lookup failed by testing `if (type_id < 0)`. However, `type_id` was declared as `u32` (unsigned), making the comparison always false since an unsigned integer can never be negative. This means error conditions from `btf_find_by_name_kind()` are silently ignored, and negative error codes wrap around to large positive values, potentially leading to invalid memory access or incorrect scheduler initialization.

## Root Cause

The `btf_find_by_name_kind()` function returns a signed type capable of returning negative error codes, but the variable storing its result was declared as `u32`. When a negative error code is assigned to an unsigned variable, it wraps around to a large positive value, bypassing the error check intended to catch initialization failures.

## Fix Summary

Change the type of `type_id` from `u32` to `s32` to properly capture and handle negative error codes returned by `btf_find_by_name_kind()`. This allows the error check `if (type_id < 0)` to function correctly and prevent use of invalid type identifiers.

## Triggering Conditions

This bug occurs during BPF sched_ext initialization when `btf_find_by_name_kind()` fails to locate the "task_struct" BTF type in the kernel's BTF metadata. The failure can happen when:
- The BTF data is corrupted or incomplete during kernel boot
- Memory allocation failures during BTF processing  
- Invalid or missing BTF metadata in the kernel image
- The unsigned `type_id` variable receives a negative error code (e.g., -ENOENT, -EINVAL) from `btf_find_by_name_kind()`, which wraps to a large positive value
- The subsequent `if (type_id < 0)` check fails, and the large positive value is used as a valid BTF type ID
- This causes `btf_type_by_id()` to access invalid memory or return incorrect type information, leading to scheduler initialization corruption

## Reproduce Strategy (kSTEP)

This bug is challenging to reproduce in kSTEP as it requires BTF initialization failure, which happens early in kernel boot before kSTEP drivers run. A direct reproduction would need:
- Use a single CPU setup (CPU 0 reserved for driver)
- In `setup()`, attempt to trigger BTF-related operations that could expose the type mismatch
- Since the bug is in initialization code, consider using kSTEP's kernel module loading facilities to simulate sched_ext registration
- In `run()`, use `kstep_tick_repeat(1)` to ensure scheduler is active, then attempt operations that would trigger sched_ext BTF verification
- Monitor for memory corruption indicators or unexpected scheduler behavior using `on_tick_begin()` callback to log scheduler state
- Check for large positive type_id values (>0x80000000) that indicate wrapped negative error codes
- Use `kstep_fail()` if unexpected positive type_ids are observed where negative values should occur
- Note: Full reproduction may require kernel modification or BTF corruption simulation beyond standard kSTEP capabilities
