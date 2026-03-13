# NUMA: Memory Leak from Concurrent vma->numab_state Initialization

**Commit:** `5f1b64e9a9b7ee9cfd32c6b2fab796e29bfed075`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.13-rc2
**Buggy since:** v6.4-rc1 (commit ef6a22b70f6d "sched/numa: apply the scan delay to every new vma")

## Bug Description

The NUMA balancing subsystem in the Linux kernel performs periodic scanning of a process's virtual memory areas (VMAs) to determine optimal NUMA placement for memory pages. This scanning is performed by `task_numa_work()`, which is invoked as a task_work callback during the syscall exit path (`syscall_exit_to_user_mode` → `task_work_run` → `task_numa_work`). For each VMA encountered during the scan, the function lazily initializes a per-VMA NUMA balancing state structure (`struct vma_numab_state`) if one does not already exist.

The bug is a race condition that causes a memory leak when multiple threads sharing the same address space (and therefore the same VMAs) concurrently execute `task_numa_work()`. Although the NUMA scanning code uses a `cmpxchg` on `mm->numa_next_scan` to nominally ensure only one thread scans per `numa_scan_period`, a timing window exists where a thread from one scan period can overlap with a thread entering the next scan period. When two or more threads simultaneously observe `vma->numab_state == NULL`, each independently allocates a new `struct vma_numab_state` via `kzalloc()` and directly assigns it to `vma->numab_state`. The second (and any subsequent) assignment overwrites the pointer without freeing the previous allocation, causing a memory leak.

This bug was discovered by Jiwei Sun at Lenovo and reported by Adrian Huang. It was consistently reproducible using LTP's hackbench with the `thread` argument (which creates threads sharing the same address space), specifically with the command `hackbench 20 thread 1000` which creates 800 threads (20 groups × 40 threads). The bug was verified on three different large servers (448-core, 256-core, and 192-core). Notably, using `hackbench 50 process 1000` (which uses `fork()` and thus separate address spaces via COW) could *not* reproduce the issue, confirming that the shared VMA is the critical factor.

## Root Cause

The root cause is a classic TOCTOU (time-of-check-time-of-use) race in `task_numa_work()` in `kernel/sched/fair.c`. The buggy code path is:

```c
/* Initialise new per-VMA NUMAB state. */
if (!vma->numab_state) {
    vma->numab_state = kzalloc(sizeof(struct vma_numab_state), GFP_KERNEL);
    if (!vma->numab_state)
        continue;
    vma->numab_state->start_scan_seq = mm->numa_scan_seq;
    vma->numab_state->next_scan = now + msecs_to_jiffies(sysctl_numa_balancing_scan_delay);
    ...
}
```

The check `if (!vma->numab_state)` and the assignment `vma->numab_state = kzalloc(...)` are not atomic. When multiple threads sharing the same `mm_struct` (and therefore the same VMAs) execute this code path concurrently, the following interleaving can occur:

1. **Thread A** on CPU X reads `vma->numab_state` and sees `NULL`.
2. **Thread B** on CPU Y reads `vma->numab_state` and also sees `NULL` (before Thread A completes its allocation).
3. **Thread A** calls `kzalloc()`, which returns pointer `P1`, and assigns `vma->numab_state = P1`.
4. **Thread B** calls `kzalloc()`, which returns pointer `P2`, and assigns `vma->numab_state = P2`, overwriting `P1`.
5. The memory pointed to by `P1` is now leaked — no reference to it remains.

The existing concurrency control in `task_numa_work()` uses `try_cmpxchg(&mm->numa_next_scan, &migrate, next_scan)` to ensure that only one thread proceeds with VMA scanning per `numa_scan_period`. However, as Raghavendra K T from AMD explained in the discussion thread, this guard is insufficient when there are hundreds of threads. The `numa_scan_period` can expire between the time one thread wins the `cmpxchg` and the time it reaches the `numab_state` allocation. When the period expires, a new thread from a different scan period can pass the `cmpxchg` gate and begin scanning the same VMAs, creating the race window.

The key insight is that the `cmpxchg` on `mm->numa_next_scan` only prevents multiple threads from scanning in the *same* period. It does not prevent a thread from a *subsequent* period from racing with a thread from the *current* period that has not yet completed its `numab_state` allocation. With 800 threads and very short scan periods on large-core systems, this race window is readily exploitable.

## Consequence

The primary consequence is a kernel memory leak. Each time the race is triggered, a 64-byte `struct vma_numab_state` allocation is leaked. The `kmemleak` tool reports these as unreferenced objects with the allocation backtrace:

```
unreferenced object 0xffff888cd8ca2c40 (size 64):
  comm "hackbench", pid 17142, jiffies 4299780315
  backtrace (crc bff18fd4):
    [<ffffffff81419a89>] __kmalloc_cache_noprof+0x2f9/0x3f0
    [<ffffffff8113f715>] task_numa_work+0x725/0xa00
    [<ffffffff8110f878>] task_work_run+0x58/0x90
    [<ffffffff81ddd9f8>] syscall_exit_to_user_mode+0x1c8/0x1e0
    [<ffffffff81dd78d5>] do_syscall_64+0x85/0x150
    [<ffffffff81e0012b>] entry_SYSCALL_64_after_hwframe+0x76/0x7e
```

With workloads like hackbench that create hundreds of threads continuously performing syscalls, the leak can be significant. The reports indicate 480-665 new suspected memory leaks per run, each being a 64-byte `vma_numab_state` structure. Over time and with repeated workloads, this could contribute to meaningful memory pressure, especially on long-running systems or servers with high-thread-count applications.

Additionally, beyond the pure memory leak, the overwriting of `vma->numab_state` means that initialization data written by the first thread (such as `start_scan_seq`, `next_scan`, and `pids_active_reset`) is lost. The VMA ends up with a freshly zero-allocated state from the second thread, which is then re-initialized. While this is unlikely to cause functional corruption (since the new state is immediately initialized), it could potentially affect the timing of NUMA scan delays for the affected VMAs.

## Fix Summary

The fix, implemented in v2 of the patch following review feedback from Vlastimil Babka (SUSE), replaces the non-atomic check-and-assign with an atomic `cmpxchg` operation. Instead of directly assigning to `vma->numab_state`, the code now:

1. Allocates the `vma_numab_state` structure into a local pointer `ptr`.
2. Uses `cmpxchg(&vma->numab_state, NULL, ptr)` to atomically install the pointer only if `vma->numab_state` is still `NULL`.
3. If `cmpxchg` returns a non-NULL value (meaning another thread won the race and already installed a state), the locally allocated structure is freed with `kfree(ptr)` and the loop continues to the next VMA.

This approach is superior to the v1 approach (which added a per-VMA mutex `numab_state_lock` to `struct vm_area_struct`). The `cmpxchg` solution is lockless, does not increase the size of `struct vm_area_struct` (a highly performance-sensitive structure), and does not require changes to `mm_types.h` or `mm.h`. Vlastimil Babka suggested this approach in his review of v1, and Raghavendra K T from AMD confirmed this was the right direction. The fix only touches `kernel/sched/fair.c`.

The fix is correct and complete because `cmpxchg` is a single atomic compare-and-swap operation that ensures exactly one thread's allocation "wins" the race. All other threads detect the loss and clean up their allocations. The subsequent initialization of `start_scan_seq`, `next_scan`, `pids_active_reset`, and `prev_scan_seq` is performed only by the winning thread, avoiding any initialization races.

## Triggering Conditions

The following conditions are required to trigger the bug:

- **Kernel version:** v6.4-rc1 through v6.13-rc1 (any kernel containing commit ef6a22b70f6d but not the fix).
- **CONFIG_NUMA_BALANCING=y:** The kernel must be compiled with NUMA balancing support enabled.
- **Multi-threaded process with shared address space:** The workload must use threads (not processes) so that multiple tasks share the same `mm_struct` and VMAs. The `hackbench thread` mode creates exactly this scenario. Using `hackbench process` mode (which uses `fork()`) does NOT trigger the bug because COW gives each process its own copy of VMAs.
- **Large number of threads:** The bug was reliably reproduced with 800 threads (20 groups × 40 threads). A high thread count increases the probability that `numa_scan_period` expires while one thread is still between the `numa_next_scan` gate and the `numab_state` allocation.
- **High core count:** The bug was consistently reproduced on 192-core, 256-core, and 448-core servers. More CPUs increase the chance of truly concurrent execution of `task_numa_work()` by different threads.
- **New VMAs being scanned:** The race only occurs when `vma->numab_state` is NULL, which means the VMA has not yet been scanned by NUMA balancing. This happens for newly created VMAs or VMAs being scanned for the first time.
- **Timing:** The race window is between the `try_cmpxchg` on `mm->numa_next_scan` and the assignment to `vma->numab_state`. One thread must pass the `numa_next_scan` gate in scan period N, and another thread must pass it in scan period N+1 before the first thread completes its `numab_state` allocation. Short `numa_scan_period` values and heavy thread contention make this more likely.

The bug is non-deterministic but highly reproducible on large-core systems with the right workload parameters. It was verified as consistently reproducible across multiple server configurations.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. It is placed in `drivers_unplanned` for the following fundamental reasons:

### 1. Why kSTEP Cannot Reproduce This Bug

The bug occurs in `task_numa_work()`, which is a task_work callback invoked during the userspace syscall exit path (`syscall_exit_to_user_mode` → `task_work_run`). This function operates on the `mm_struct` (memory management structure) and VMAs (virtual memory areas) of real userspace processes. The race requires:

- **Real userspace threads sharing an `mm_struct`:** kSTEP creates kernel threads via `kstep_kthread_create()` or simulated tasks via `kstep_task_create()`. Kernel threads do not have their own `mm_struct` (they borrow the previous task's `active_mm`), and kSTEP tasks are not real userspace processes with memory mappings. The bug specifically requires multiple threads belonging to the same userspace process, sharing the same VMAs — a construct that kSTEP fundamentally cannot create.

- **Real VMA structures with NUMA balancing state:** `task_numa_work()` iterates over `vma_next(&vmi)` in the process's VMA tree and checks/initializes `vma->numab_state`. Creating VMAs requires userspace memory operations (mmap, brk, etc.) which kSTEP cannot perform since it cannot intercept userspace syscalls and runs only kernel modules.

- **task_work callback mechanism:** `task_numa_work()` is scheduled via `task_work_add()` and runs during `task_work_run()` at syscall exit. kSTEP has no mechanism to trigger this path since its tasks don't perform userspace syscalls.

- **NUMA balancing scanning:** The NUMA balancing subsystem scans VMAs and installs NUMA hint page faults to track memory access patterns. This entire mechanism depends on the mm subsystem, page tables, and userspace memory access — all of which are outside kSTEP's scope.

### 2. What Would Need to Be Added to kSTEP

To reproduce this bug, kSTEP would need fundamental architectural changes:

- **Real userspace process creation:** The ability to create actual userspace processes with shared address spaces (threads) inside QEMU, including full mm_struct and VMA setup. This would require a userspace init program or a mechanism to spawn user-mode processes from the kernel module.

- **VMA manipulation APIs:** Functions like `kstep_vma_create(mm, start, end, flags)` to create VMAs in a process's address space, or at minimum the ability to trigger `task_numa_work()` on behalf of a task with a valid mm_struct containing VMAs.

- **task_work triggering:** A mechanism to force `task_work_run()` execution on specific tasks, which normally only happens during syscall exit or signal handling.

- **NUMA balancing hooks:** The ability to control `mm->numa_scan_seq`, `mm->numa_next_scan`, and `sysctl_numa_balancing_scan_delay` to create the precise timing window needed for the race.

These are not minor extensions — they require fundamentally new capabilities for kSTEP to handle userspace memory management, which is outside its core architecture of kernel-level task scheduling control.

### 3. Alternative Reproduction Methods

The bug can be reliably reproduced outside kSTEP by:

- **LTP hackbench:** Run `hackbench 20 thread 1000` on a multi-core system (ideally 192+ cores) with NUMA balancing enabled (`/proc/sys/kernel/numa_balancing = 1`). Enable kmemleak (`CONFIG_DEBUG_KMEMLEAK=y`) and check `/sys/kernel/debug/kmemleak` after the test completes.

- **Custom test program:** Write a program that creates hundreds of threads (using `pthread_create`, which shares the address space) and has them perform syscalls in tight loops to trigger frequent `task_numa_work()` invocations. The threads should allocate memory to create VMAs that NUMA balancing will scan.

- **Kernel debugging:** Add `pr_warn()` calls in `task_numa_work()` before and after the `vma->numab_state` assignment to observe concurrent access patterns. The `kmemleak` backtrace clearly identifies the leaked allocations.
