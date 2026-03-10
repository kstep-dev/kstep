# sched/psi: Fix OOB write when writing 0 bytes to PSI files

- **Commit:** 6fcca0fa48118e6d63733eb4644c6cd880c15b8f
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

When a user issues a write() system call with count parameter set to 0 on any file under /proc/pressure/, the `psi_write()` function attempts to NUL-terminate a buffer at index `buf_size - 1`. Since `buf_size` is computed as `min(nbytes, sizeof(buf))`, a 0-byte write results in `buf_size` being 0, causing an out-of-bounds write at `buf[-1]`. This memory corruption can lead to kernel crashes or security vulnerabilities.

## Root Cause

The `psi_write()` function accesses `buf[buf_size - 1]` for NUL-termination without first validating that `buf_size` is non-zero. When `nbytes` is 0, the minimum of 0 and the buffer size is 0, resulting in an invalid array access at `buf[-1]`.

## Fix Summary

The fix adds an early validation check that returns `-EINVAL` if `nbytes` is 0, preventing the out-of-bounds write from occurring. This ensures the function only proceeds with buffer operations when valid data is provided.

## Triggering Conditions

The bug is triggered by a write() system call with count=0 to any PSI file under /proc/pressure/. The specific conditions are:
- PSI subsystem must be enabled (psi_disabled static branch is false)
- User performs write() syscall with nbytes=0 to /proc/pressure/memory, /proc/pressure/cpu, or /proc/pressure/io
- The psi_write() function calculates buf_size = min(0, 32) = 0, then accesses buf[buf_size-1] = buf[-1]
- This causes an out-of-bounds write one byte before the buf array, corrupting kernel memory
- No specific scheduler state, task configuration, or timing conditions are required
- The bug occurs deterministically on every 0-byte write to PSI files

## Reproduce Strategy (kSTEP)

Since this is a userspace API bug rather than scheduler state corruption, reproduction focuses on triggering the faulty write path:
- Setup: Ensure PSI is enabled in kernel config (no special CPU topology needed, can use 2 CPUs)  
- Create a simple task using kstep_task_create() to establish basic kernel context
- Use kstep_write("/proc/pressure/memory", "", 0) to write 0 bytes to PSI file
- Also test kstep_write("/proc/pressure/cpu", "", 0) and kstep_write("/proc/pressure/io", "", 0)
- Add memory corruption detection by examining kernel memory before/after the write
- In setup(), allocate known patterns around potential corruption sites to detect the OOB write
- Log any kernel panics, memory corruption warnings, or unexpected behavior in run()
- Success indicator: kernel crash, memory corruption detected, or KASAN/UBSAN warnings
- On fixed kernel: write should return -EINVAL instead of causing memory corruption
