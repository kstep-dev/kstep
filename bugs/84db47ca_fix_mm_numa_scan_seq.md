# Fix mm numa_scan_seq based unconditional scan

- **Commit:** 84db47ca7146d7bd00eb5cf2b93989a971c84650
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA, fair scheduling

## Bug Description

NUMA Balancing performs unconditional PTE updates to trap NUMA hinting faults for all VMA pages during an initial phase (while the process's global `mm->numa_scan_seq` counter is less than 2). However, if a VMA is created after the process's `mm->numa_scan_seq` counter has already exceeded 2, that VMA is incorrectly excluded from unconditional scanning. This causes newly created VMAs to never receive initial PTE protection updates, preventing the kernel from learning their access patterns and optimizing NUMA page placement.

## Root Cause

The bug occurs because the code checks a global `mm->numa_scan_seq` counter to determine whether to perform unconditional VMA scans, without considering when each individual VMA was created. For VMAs created early in the process's lifetime (before `mm->numa_scan_seq >= 2`), the check succeeds and they receive unconditional scans. But for VMAs created later—when `mm->numa_scan_seq` is already > 2—the condition fails unconditionally, bypassing the opportunity to scan them.

## Fix Summary

The fix records the value of `mm->numa_scan_seq` at the time each VMA is created in a per-VMA `start_scan_seq` field. The unconditional scan condition is then changed from checking `mm->numa_scan_seq < 2` to checking `(mm->numa_scan_seq - vma->start_scan_seq) < 2`. This allows each VMA to receive unconditional scans for the first two scan iterations after its creation, regardless of when the VMA was created relative to the global scan counter.

## Triggering Conditions

The bug occurs in the NUMA balancing subsystem when VMAs are created after the process's global scan counter advances. Specific conditions:
- NUMA balancing must be enabled (`/proc/sys/kernel/numa_balancing` = 1)
- Process must have `mm->numa_scan_seq >= 2` before VMA creation
- New VMA creation via mmap(), fork(), or similar memory allocation
- NUMA scanner task must attempt to scan the late-created VMA
- Without the fix, `task_numa_work()` checks `mm->numa_scan_seq < 2` and skips unconditional PTE protection updates
- Results in the VMA never receiving initial scanning, preventing NUMA page migration optimization

## Reproduce Strategy (kSTEP)

Reproduce by creating tasks that advance the scan counter, then allocate new VMAs:
- Use at least 2 CPUs (CPU 0 reserved for driver) with NUMA topology via `kstep_topo_set_node()`
- In `setup()`: Enable NUMA balancing via `kstep_sysctl_write("kernel/numa_balancing", "1")`
- Create initial task with `kstep_task_create()` and memory allocations to advance `mm->numa_scan_seq`
- Use `kstep_tick_repeat()` with sufficient iterations to trigger multiple NUMA scans (scan_seq >= 2)
- Create additional tasks/VMAs after scan_seq advancement via `kstep_task_fork()` 
- Monitor `on_tick_begin()` to track when NUMA scanner runs and observe PTE update counts
- Check kernel logs for absence of PTE updates on late-created VMAs vs early ones
- Success: Late VMAs show zero NUMA PTE updates; Fixed kernel shows equal treatment
