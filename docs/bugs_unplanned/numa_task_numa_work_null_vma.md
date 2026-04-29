# NUMA: NULL pointer dereference in task_numa_work() on empty address space

**Commit:** `9c70b2a33cd2aa6a5a59c5523ef053bd42265209`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.12-rc6
**Buggy since:** v6.3-rc1 (introduced by commit `214dbc428137` "sched: convert to vma iterator")

## Bug Description

The `task_numa_work()` function in the NUMA balancing subsystem can dereference a NULL pointer when iterating over a process's virtual memory areas (VMAs). This function is responsible for scanning a process's address space to identify pages that should be migrated closer to the NUMA node where they are most accessed. It runs as a `task_work` callback, invoked during the return-to-userspace path (via `task_work_run()` → `do_notify_resume()`).

The bug was introduced when commit `214dbc428137` ("sched: convert to vma iterator") converted the VMA iteration loop from a standard `for` loop using the maple tree API (`mas_find()`) to a `do { ... } for_each_vma(vmi, vma)` construct. The original `for` loop checked the `vma` pointer before each iteration body, including the first one. The replacement `do-while` pattern unconditionally executes the loop body at least once, regardless of whether `vma` is NULL.

Under normal conditions, a process always has at least one VMA (for its stack, heap, or code segment), so the initial `vma_next()` call returns a valid pointer and the do-while loop works correctly. However, the `stress-ng-vm-segv` stress test deliberately calls `munmap()` to unmap the **entire** address space of a child process, leaving zero VMAs. When `task_numa_work()` is triggered for such a process (because NUMA balancing work was already queued before the munmap), the VMA iterator returns NULL even after resetting to address 0, and the do-while loop body dereferences the NULL pointer, causing a kernel oops.

The crash was observed on an ARM64 system (aarch64) running the `stress-ng-vm-segv` workload, manifesting as an "Unable to handle kernel NULL pointer dereference at virtual address 0000000000000020" inside `vma_migratable()`, which is the first function called in the loop body on the `vma` pointer.

## Root Cause

The root cause is the incorrect loop construct used in `task_numa_work()` after commit `214dbc428137`. Specifically, the conversion changed:

**Before (original `for` loop with maple tree):**
```c
for (; vma; vma = mas_find(&mas, ULONG_MAX)) {
    if (!vma_migratable(vma) || ...)
        continue;
    ...
}
```

**After (buggy `do-while` with VMA iterator):**
```c
do {
    if (!vma_migratable(vma) || ...)  // dereferences vma
        continue;
    ...
} for_each_vma(vmi, vma);
```

The `for_each_vma` macro (defined in `include/linux/mm.h`) expands to:
```c
while ((vma = vma_next(&vmi)) != NULL)
```

So the combined `do { ... } for_each_vma(vmi, vma)` expands to:
```c
do {
    // body that dereferences vma
} while ((vma = vma_next(&vmi)) != NULL);
```

This is a standard C `do-while` loop that **always executes the body before checking the condition**. The initial value of `vma` is set by code preceding the loop:

```c
vma_iter_init(&vmi, mm, start);
vma = vma_next(&vmi);
if (!vma) {
    reset_ptenuma_scan(p);
    start = 0;
    vma_iter_set(&vmi, start);
    vma = vma_next(&vmi);  // Still NULL if no VMAs exist
}

do {
    if (!vma_migratable(vma) || ...)  // NULL deref here
```

When a process has no VMAs (after unmapping its entire address space), both calls to `vma_next()` return NULL. The code enters the `do-while` loop with `vma == NULL`, and the first statement in the loop body calls `vma_migratable(vma)`, which dereferences `vma` to access `vma->vm_mm`, causing the NULL pointer dereference.

The original `for` loop pattern correctly handled this case because the condition `vma` was checked **before** entering the loop body. The `do-while` conversion inadvertently removed this safety check.

## Consequence

The immediate consequence is a kernel NULL pointer dereference, which manifests as a fatal kernel oops/crash. On the system where this was discovered (ARM64), the crash trace shows:

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000020
pc : vma_migratable+0x1c/0xd0
lr : task_numa_work+0x1ec/0x4e0
```

The virtual address `0x20` corresponds to the offset of the `vm_mm` field (or similar early field) within `struct vm_area_struct`, confirming that `vma` was NULL and the code tried to read `vma->vm_mm` at offset 0x20 from NULL.

This is a **kernel crash** that triggers the system's crash dump mechanism (`Starting crashdump kernel...`). On systems without kdump configured, this results in a hard system hang or reboot. The crash affects any NUMA-capable system (multi-node topology) running workloads that can unmap an entire process's address space while NUMA balancing is active. While the `stress-ng-vm-segv` stress test is the known trigger, any program that calls `munmap()` on its entire address space at the right moment could potentially trigger this bug.

The bug is particularly insidious because it depends on timing: `task_numa_work()` must be queued as a `task_work` callback **before** the `munmap()`, but executed **after** the `munmap()` completes (during the return-to-userspace path). This timing window exists naturally in the kernel's task_work mechanism, where NUMA scanning work is scheduled periodically via `task_tick_numa()` and executed later via `task_work_run()`.

## Fix Summary

The fix replaces the buggy `do { ... } for_each_vma(vmi, vma)` construct with a standard `for` loop that checks `vma` before each iteration:

**Before (buggy):**
```c
do {
    if (!vma_migratable(vma) || ...)
        continue;
    ...
} for_each_vma(vmi, vma);
```

**After (fixed):**
```c
for (; vma; vma = vma_next(&vmi)) {
    if (!vma_migratable(vma) || ...)
        continue;
    ...
}
```

This restores the original semantics that existed before commit `214dbc428137`. The `for` loop checks `vma != NULL` before the first iteration, so if the process has no VMAs, the loop body is never entered. The loop advancement `vma = vma_next(&vmi)` is placed in the `for` statement's increment expression, and `vma_next()` naturally returns NULL when there are no more VMAs, terminating the loop.

The fix is minimal (2 lines changed) and correct because it preserves all the existing loop semantics: the `continue` statements still skip to the next VMA (via the `for` increment), the `break` statements still exit the loop, and the post-loop code that checks `if (!vma)` to determine whether the scan is complete still works identically. The `for_each_vma` macro is no longer used, but the explicit `vma_next()` call in the `for` increment serves the same purpose. This fix was reviewed by Liam R. Howlett (the original author of the VMA iterator conversion) and merged by Peter Zijlstra with a `Cc: stable@vger.kernel.org # v6.2+` tag for backporting to stable kernels.

## Triggering Conditions

The bug requires the following precise conditions to trigger:

1. **NUMA topology**: The system must have NUMA balancing enabled (`CONFIG_NUMA_BALANCING=y` and `sysctl kernel.numa_balancing = 1`). This is the default on multi-node NUMA systems.

2. **NUMA scanning scheduled**: The task must have `task_numa_work()` queued as a `task_work` callback. This is done by `task_tick_numa()` during scheduler ticks when `p->mm->numa_next_scan` time has been reached and the task's `p->numa_work.func` is not already set. The NUMA scanning is periodic, controlled by sysctl parameters (`numa_balancing_scan_delay_ms`, `numa_balancing_scan_period_min_ms`, etc.).

3. **Empty address space**: The task must have **zero VMAs** in its `mm_struct` at the time `task_numa_work()` executes. This occurs when the task calls `munmap()` to unmap its entire address space. The `stress-ng-vm-segv` test does exactly this: it forks a child process, and the child unmaps its entire address space to trigger a SIGSEGV on return to userspace.

4. **Timing**: The `task_numa_work()` callback must be scheduled (by `task_tick_numa()`) **before** the `munmap()` call, but executed (by `task_work_run()` in the return-to-userspace path) **after** the `munmap()` completes. Since `task_work_run()` is called in `do_notify_resume()` (part of the syscall return path), the timing is: `munmap()` syscall returns → `do_notify_resume()` → `task_work_run()` → `task_numa_work()`. The munmap has already removed all VMAs, but the task_work callback was already queued.

5. **No other protections**: The code does acquire `mmap_read_trylock(mm)` before iterating VMAs, but by this point the `munmap()` has already completed and released the mmap lock. The mmap lock only prevents concurrent modification, not a state where all VMAs are already gone.

The probability of triggering this bug depends on NUMA balancing scan timing aligning with the `munmap()` operation. With `stress-ng-vm-segv` running many iterations, the timing window is hit reliably within minutes to hours of stress testing.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

### 1. Why kSTEP cannot reproduce this bug

The bug requires a **real userspace process** with a valid `mm_struct` containing VMAs that can be removed via `munmap()`. kSTEP's task management creates kernel-controlled tasks (kthreads), which have `p->mm == NULL`. The `task_numa_work()` function checks `p->mm` early and returns immediately if it is NULL:

```c
static void task_numa_work(struct callback_head *work)
{
    ...
    struct mm_struct *mm = p->mm;
    ...
    if (!mm || (p->flags & PF_EXITING) || work->next != work)
        return;
```

Since kSTEP tasks are kernel threads with no `mm_struct`, `task_numa_work()` would never proceed past this check, making it impossible to reach the buggy VMA iteration code.

### 2. The specific capabilities kSTEP lacks

- **Userspace memory management**: kSTEP cannot create tasks with a valid `mm_struct` containing VMAs. There is no `kstep_task_mmap()` or `kstep_task_munmap()` API.
- **Syscall interception**: The bug trigger requires executing a `munmap()` syscall from the task's context. kSTEP explicitly states it "Cannot intercept userspace syscalls directly; tasks are kernel-controlled."
- **task_work callbacks**: `task_numa_work()` is registered via `task_work_add()` and executed during return-to-userspace (`do_notify_resume()`). kSTEP tasks, being kernel threads, never take the return-to-userspace path and thus never execute `task_work_run()`.
- **NUMA scanning initiation**: `task_tick_numa()` only triggers NUMA scanning work for tasks with valid `mm_struct` (`if (p->mm && ...)`). kSTEP tasks would never have NUMA work queued.

### 3. What would need to be added to kSTEP

To support this class of bugs, kSTEP would need fundamental architectural changes:
- **Full userspace process creation**: Ability to fork real userspace processes with their own `mm_struct`, page tables, and VMAs. This goes far beyond kSTEP's kernel-thread-based task model.
- **Memory management APIs**: `kstep_task_mmap(p, addr, len, prot)` and `kstep_task_munmap(p, addr, len)` to manipulate a task's virtual address space.
- **task_work injection**: API to queue `task_work` callbacks (like `task_numa_work`) for specific tasks and trigger their execution.
- **Return-to-userspace simulation**: A mechanism to simulate the `do_notify_resume()` path where `task_work_run()` is called.

These are not minor extensions — they require fundamentally rethinking kSTEP's execution model from kernel-thread-based to full-process-based.

### 4. Alternative reproduction methods

The bug can be reproduced outside kSTEP using the following methods:

**Method 1: stress-ng (original trigger)**
```bash
# On a NUMA system with numa_balancing enabled
sysctl -w kernel.numa_balancing=1
stress-ng --vm-segv 4 --timeout 3600
```
This runs the `stress-ng-vm-segv` stressor which repeatedly forks children that unmap their entire address space. The crash typically occurs within minutes to hours on a NUMA system.

**Method 2: Custom userspace program**
Write a program that:
1. Forks a child process
2. The child maps a large memory region (to trigger NUMA scanning)
3. Runs long enough for `task_tick_numa()` to queue `task_numa_work()`
4. Calls `munmap()` on the entire address space
5. The `task_work_run()` in the syscall return path triggers the NULL deref

**Method 3: Direct kernel testing**
On a NUMA-enabled kernel, create a program that calls `munmap(0, TASK_SIZE)` after enough execution time for NUMA scanning to be queued. The timing window between NUMA work being queued and the munmap completing is the critical factor.
