# NUMA: VMA Scan Starvation Due to Cleared pids_active

**Commit:** `f22cde4371f3c624e947a35b075c06c771442a43`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.12-rc1
**Buggy since:** v6.4-rc1 (introduced by commit fc137c0ddab2 "sched/numa: enhance vma scanning logic")

## Bug Description

The Linux kernel's NUMA balancing subsystem periodically scans Virtual Memory Areas (VMAs) to detect which NUMA node a task's memory is being accessed from, and migrates pages to the local NUMA node when beneficial. The scanning is driven by `task_numa_work()`, which iterates over a process's VMAs and sets page table entries to `prot_none` (no permissions). When the task subsequently accesses those pages, the resulting page faults provide information about which CPU (and thus which NUMA node) is accessing each page. This information guides the NUMA balancer's page migration decisions.

Commit fc137c0ddab2 ("sched/numa: enhance vma scanning logic") introduced an optimization that tracks which PIDs have recently accessed each VMA via a `pids_active` bitmask in `vma->numab_state`. The function `vma_is_accessed()` was added to gate VMA scanning: if the current task's PID is not recorded in the VMA's `pids_active` array, the VMA is skipped during the scan. This reduces overhead by avoiding scans of VMAs that the current task hasn't recently touched.

However, this optimization introduced a starvation bug. The `pids_active` array is periodically cleared (every `VMA_PID_RESET_PERIOD = 4 * sysctl_numa_balancing_scan_delay`). If a process accesses a VMA early in its lifetime, then sleeps or is otherwise inactive for long enough that the `pids_active` array is cleared, the process can never trigger a NUMA scan for that VMA again. The only conditions under which `vma_is_accessed()` returned true were: (1) the first two scan sequences (controlled by `start_scan_seq`), (2) the PID being in `pids_active`, or (3) the scan offset being past the VMA's start (indicating a partially completed scan). After the initial two unconditional scans, with `pids_active` cleared and no partial scan in progress, `vma_is_accessed()` permanently returns false for that VMA.

This was observed in practice on a 320-CPU / 2-socket Intel system running SPECcpu omnetpp_r, where PMU events showed sustained ~500 MB/s remote NUMA node memory access for approximately 20 seconds on a few cores, causing significant performance variance and degradation.

## Root Cause

The root cause is in the `vma_is_accessed()` function in `kernel/sched/fair.c`. Before the fix, this function had three conditions under which it would return `true` (allowing the VMA to be scanned):

1. **Unconditional initial scans**: `(READ_ONCE(current->mm->numa_scan_seq) - vma->numab_state->start_scan_seq) < 2` — The first two scan sequences unconditionally scan all VMAs to establish baseline NUMA fault information.

2. **PID access check**: The function checks whether the current task's PID (hashed to a bit position) is set in `vma->numab_state->pids_active[0] | pids_active[1]`. This bitmask is populated by `vma_set_access_pid_bit()` when a NUMA page fault occurs on the VMA, and is periodically cleared every `VMA_PID_RESET_PERIOD`.

3. **Partial scan continuation**: `mm->numa_scan_offset > vma->vm_start` — If the scanner has already started scanning past this VMA's start address, it continues regardless of PID access, to avoid leaving VMAs half-scanned.

The critical gap is what happens after the initial two scans are complete, the `pids_active` array has been cleared, and no partial scan is in progress. In a process with few threads (especially a single-threaded process like SPECcpu omnetpp_r), the scenario unfolds as follows:

- The process runs and accesses its VMAs. During the first two `numa_scan_seq` iterations, all VMAs are scanned unconditionally, and `prot_none` faults provide access information.
- The process then sleeps or becomes idle for an extended period. During this time, the periodic clearing of `pids_active` removes the process's PID from the bitmask.
- When the process wakes up and `task_numa_work()` runs again, `vma_is_accessed()` returns false for the VMA: the initial 2-scan window is past, the PID is not in `pids_active`, and `numa_scan_offset` is 0 (reset at the start of a new scan pass).
- Because `prot_none` was never set on the VMA's pages in this new scan, the NUMA page fault handler never fires, so `vma_set_access_pid_bit()` is never called, and `pids_active` remains clear. This creates a permanent feedback loop: no scan → no faults → no PID bits → no scan.

In a multi-threaded process, other threads might trigger faults and set `pids_active` bits, which could indirectly allow the stalled thread to resume scanning. But for processes with few threads (particularly single-threaded ones), there are no other threads to break the cycle. The `get_nr_threads(current)` value is 1 for single-threaded processes, making this starvation especially severe.

The result is that the NUMA balancer loses all information about the process's memory access patterns. Pages that should be migrated to the local NUMA node remain on a remote node, causing sustained remote memory access with the associated latency and bandwidth penalties.

## Consequence

The observable impact is significant performance degradation on NUMA systems due to excessive remote memory access. On the reported system (320-CPU / 2-socket Intel), PMU events showed a few cores sustaining approximately 500 MB/s of remote NUMA node memory reads for around 20 seconds at a time. This remote access is dramatically slower than local NUMA access, causing:

- **High core-to-core performance variance**: Some cores run efficiently with local memory access while others are bottlenecked by remote access, leading to load imbalance and unpredictable execution times.
- **Overall throughput degradation**: Benchmarks showed ~6-15% improvement in system time when the fix was applied (e.g., autonumabench NUMA01 showed 14.92% improvement in system time on AMD, 6.11% on Intel), confirming the significant overhead of the bug.
- **Starvation of NUMA information gathering**: Without VMA scanning, the NUMA balancer cannot make informed page migration decisions. Even if the system eventually recovers (e.g., through a new scan sequence resetting state), there is a prolonged window where memory placement is suboptimal.

This is not a crash or hang — it is a performance regression that manifests as sustained remote NUMA memory access in workloads with few threads that have periodic sleep/wake patterns. The bug is particularly impactful for process-based workloads like SPECcpu, where each benchmark instance is a separate single-threaded process.

## Fix Summary

The fix adds a new condition to `vma_is_accessed()` in `kernel/sched/fair.c` that acts as a safety net against scan starvation. Specifically, it adds the following check just before the final `return false`:

```c
if (READ_ONCE(mm->numa_scan_seq) >
   (vma->numab_state->prev_scan_seq + get_nr_threads(current)))
    return true;
```

This condition compares the current global scan sequence number (`mm->numa_scan_seq`) with the last scan sequence number for this VMA (`vma->numab_state->prev_scan_seq`), plus the number of threads in the current process. The logic is: if `numa_scan_seq` has advanced by more than `get_nr_threads(current)` since the VMA was last scanned, force a scan regardless of the PID access check.

The threshold `get_nr_threads(current)` is chosen deliberately. In a multi-threaded process, there are more threads that can potentially trigger NUMA faults and populate `pids_active`, so a longer grace period before forcing a scan is acceptable. In a single-threaded process (`get_nr_threads(current) == 1`), the VMA will be forced-scanned after just one scan sequence passes without it being scanned, since there are no other threads to help. This makes the fix adaptive to the process's thread count, avoiding unnecessary forced scans in multi-threaded workloads while aggressively preventing starvation in few-thread workloads.

The fix is minimal and correct: it preserves all existing optimization behavior (PID-based scan filtering, unconditional initial scans, partial scan continuation) while adding a bounded guarantee that no VMA can be starved of scanning indefinitely. The `prev_scan_seq` field was already maintained by the existing code (updated each time a VMA is scanned), so no new bookkeeping was needed.

## Triggering Conditions

The bug requires the following specific conditions to trigger:

- **NUMA system**: At least 2 NUMA nodes with memory distributed across nodes. The bug only matters when NUMA balancing is active (`CONFIG_NUMA_BALANCING=y` and `kernel.numa_balancing=1`).
- **A process with few threads** (ideally single-threaded): Multi-threaded processes can partially work around the bug because other threads may trigger NUMA faults and set `pids_active` bits. Single-threaded or few-threaded processes are most affected.
- **Memory mapped across NUMA nodes**: The process must have VMAs whose backing pages are on a remote NUMA node (e.g., allocated on one node, then the process migrated to another).
- **Sleep/wake pattern**: The process must access its VMAs (triggering the initial unconditional scans), then sleep or remain inactive for at least `VMA_PID_RESET_PERIOD` (= `4 * sysctl_numa_balancing_scan_delay`, typically 4 * 1000ms = 4 seconds), long enough for the `pids_active` array to be cleared.
- **More than 2 scan sequences elapsed**: The unconditional initial scan window (`start_scan_seq + 2`) must be past. Since `numa_scan_seq` increments each time `reset_ptenuma_scan()` runs, this happens after 2 complete scan passes.
- **No partial scan in progress for the VMA**: The `numa_scan_offset` must not be past the VMA's `vm_start`, meaning the scanner has not yet begun processing this VMA in the current pass.

The probability of reproduction is high on NUMA systems running process-based workloads with periodic sleep phases. SPECcpu omnetpp_r is a canonical example. The bug manifests reliably once the above timing conditions are met — it is not a race condition but a deterministic starvation of the VMA scan logic.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

1. **Requires real userspace processes with mm_struct**: The NUMA balancing scanner (`task_numa_work()`) operates on the memory mappings of a real userspace process. It iterates over VMAs in the process's `mm_struct`, examines page table entries, and sets `prot_none` to trigger page faults. kSTEP creates kernel threads via `kstep_task_create()` and `kstep_kthread_create()`, which do not have a userspace `mm_struct` or VMAs. There is no way to create a kernel thread with a realistic VMA layout that the NUMA scanner would process.

2. **Requires VMA and page table manipulation**: The core of the bug is in `vma_is_accessed()`, which examines `vma->numab_state` fields including `pids_active`, `start_scan_seq`, and `prev_scan_seq`. These are populated and updated by the NUMA page fault handler and the VMA scan path. Without real VMAs, page tables, and the `prot_none` fault mechanism, these fields cannot be exercised.

3. **Requires NUMA page fault infrastructure**: The `pids_active` bitmask is populated by `vma_set_access_pid_bit()`, which is called from the NUMA page fault handler (`do_numa_page()` / `do_huge_pmd_numa_page()`). This requires actual page table entries being set to `prot_none`, followed by real memory accesses that trigger page faults. kSTEP cannot simulate this page fault pipeline.

4. **Requires real NUMA memory topology**: The bug's observable impact is remote NUMA memory access. While kSTEP can configure NUMA topology structure via `kstep_topo_*` APIs, it cannot simulate actual memory placement across NUMA nodes or the latency/bandwidth differences between local and remote NUMA access. The performance degradation that characterizes this bug is inherently tied to real NUMA hardware behavior.

5. **What would be needed in kSTEP**: To reproduce this bug, kSTEP would need fundamental new capabilities: (a) the ability to create tasks with a real `mm_struct` containing VMAs with page tables, (b) simulation of NUMA page faults including the `prot_none` → fault → `do_numa_page()` path, (c) simulation of the `task_numa_work()` callback being triggered from `task_tick_numa()`, and (d) a way to populate and clear `pids_active` bitmasks. These are not minor extensions — they would require building an entire memory management simulation layer within kSTEP.

6. **Alternative reproduction methods**: The bug can be reproduced outside kSTEP using:
   - A real 2+ NUMA-node system with `numactl` to control initial memory placement.
   - A single-threaded test program that: (a) allocates a large memory region, (b) accesses it to trigger initial NUMA scans, (c) sleeps for several seconds (longer than `VMA_PID_RESET_PERIOD`), (d) resumes and accesses the memory again.
   - Monitor with `perf stat` for remote NUMA access events, or with `trace_sched_skip_vma_numa` tracepoints to observe `vma_is_accessed()` returning false.
   - Compare behavior on buggy kernel (VMA scans stop after sleep) vs fixed kernel (VMA scans resume due to the new `prev_scan_seq` check).
   - SPECcpu omnetpp_r is the canonical reproducer as described in the commit message.
