# sched/numa: Fix the vma scan starving issue

- **Commit:** f22cde4371f3c624e947a35b075c06c771442a43
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** sched/numa

## Bug Description

The `vma_is_accessed()` function incorrectly denies VMA scans for processes with few threads that sleep for extended periods. When a process with low thread count accesses a VMA early then sleeps, the `pids_active` array (which tracks recent accesses) expires. Upon wakeup, the function fails to grant scan permission because it relies solely on the `pids_active` check, and no other threads exist to help trigger scanning. This causes persistent remote NUMA memory access (~500MB/s for ~20 seconds) and significant performance degradation, particularly in process-based workloads like SPECcpu.

## Root Cause

The `vma_is_accessed()` function determines whether to scan a VMA using the `pids_active` array, which has time-based expiration. For processes with few threads that sleep, the array clears between accesses. The function lacks a mechanism to force scanning when no threads are accessing the VMA, causing indefinite starvation of NUMA balancing information collection for such processes.

## Fix Summary

The fix adds an additional condition to `vma_is_accessed()` that forces a VMA scan when the scan sequence counter (`mm->numa_scan_seq`) has advanced by more than the number of threads since the last scan (`vma->numab_state->prev_scan_seq`). This ensures starved VMAs eventually get scanned regardless of thread activity, preventing information loss and enabling proper NUMA balancing.

## Triggering Conditions

- Process with low thread count (typically single-threaded like SPECcpu)
- VMA accessed early in process lifetime, establishing `pids_active` entries
- Extended sleep period causing `pids_active` array expiration (time-based)
- Process wakeup after `pids_active` has cleared completely
- `vma_is_accessed()` called but returns false due to empty `pids_active`
- `mm->numa_scan_seq` advances while VMA scan is denied, creating scan starvation
- Results in persistent remote NUMA access without proper page migration

## Reproduce Strategy (kSTEP)

- Setup: 2+ CPUs with NUMA topology using `kstep_topo_*` functions
- Create single-threaded task with `kstep_task_create()` 
- Force task to access VMA early (trigger initial `pids_active` population)
- Use `kstep_task_pause()` to simulate extended sleep period
- Advance scheduler time with `kstep_tick_repeat()` to expire `pids_active`
- Wake task with `kstep_task_wakeup()` and observe VMA scan behavior
- Monitor `mm->numa_scan_seq` vs `vma->numab_state->prev_scan_seq` difference
- On buggy kernel: difference exceeds thread count but no VMA scan occurs
- Use NUMA memory access patterns to demonstrate performance impact
- Verify fix allows scan when difference > `get_nr_threads(current)`
