# NUMA: Boot crash from invalid NUMA_NO_NODE in task_numa_placement()

**Commit:** `ab31c7fd2d37bc3580d9d712d5f2dfb69901fca9`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.18-rc1
**Buggy since:** v5.18-rc1 (commit 5c7b1aaf139d "sched/numa: Avoid migrating task to CPU-less node", merged in the same development window)

## Bug Description

The NUMA balancing subsystem in the Linux scheduler periodically evaluates which NUMA node a task should prefer for scheduling, based on its memory access patterns. The function `task_numa_placement()` in `kernel/sched/fair.c` is the core routine that makes this determination. It iterates over all online NUMA nodes, accumulates memory fault statistics for each node, and selects the node with the highest number of faults as the task's preferred node (`max_nid`).

Commit `5c7b1aaf139d` ("sched/numa: Avoid migrating task to CPU-less node") added a guard in `task_numa_placement()` to prevent a task from being placed on a NUMA node that has no CPUs (e.g., persistent memory (PMEM) nodes like Intel Optane DCPMM nodes). The guard calls `node_state(max_nid, N_CPU)` to check whether the selected node has any CPUs. If the node is CPU-less, the code finds the nearest node that does have CPUs and redirects `max_nid` accordingly.

However, this guard did not account for the case where `max_nid` has never been updated from its initial value. The variable `max_nid` is initialized to `NUMA_NO_NODE` (which is `-1`). If a task has no accumulated NUMA faults — for example, during very early boot before any NUMA page scanning has occurred, or for a newly forked task — the `for_each_online_node(nid)` loop in `task_numa_placement()` never finds a node with `faults > max_faults` (since `max_faults` starts at 0 and all per-node faults are also 0), so `max_nid` remains `NUMA_NO_NODE`. The subsequent call to `node_state(NUMA_NO_NODE, N_CPU)` passes `-1` as the node index to `test_bit()`, which performs an out-of-bounds memory access on the `node_states[N_CPU]` bitmap, causing a kernel crash.

This was reported by Qian Cai as a boot crash on arm64 systems. The crash occurs deterministically during early boot when NUMA balancing triggers `task_numa_placement()` for tasks that have not yet accumulated any NUMA fault statistics.

## Root Cause

The root cause is a missing bounds check on `max_nid` before it is used as an index into the `node_states[]` bitmap array.

In `task_numa_placement()`, the variable `max_nid` is declared and initialized at the top of the function:

```c
int seq, nid, max_nid = NUMA_NO_NODE;
unsigned long max_faults = 0;
```

`NUMA_NO_NODE` is defined as `-1` in `include/linux/numa.h`. The function then iterates over all online NUMA nodes to find the one with the highest fault count:

```c
for_each_online_node(nid) {
    /* ... accumulate faults for this nid ... */
    if (!ng) {
        if (faults > max_faults) {
            max_faults = faults;
            max_nid = nid;
        }
    } else if (group_faults > max_faults) {
        max_faults = group_faults;
        max_nid = nid;
    }
}
```

If `max_faults` remains 0 (because the task has no NUMA faults), none of the `faults > max_faults` comparisons succeed, and `max_nid` stays at `NUMA_NO_NODE` (-1).

The code then reaches the CPU-less node guard added by commit `5c7b1aaf139d`:

```c
/* Cannot migrate task to CPU-less node */
if (!node_state(max_nid, N_CPU)) {
```

The `node_state()` macro expands to:

```c
#define node_state(node, __state) test_bit(node, node_states[__state].bits)
```

When `max_nid` is `-1`, this becomes `test_bit(-1, node_states[N_CPU].bits)`, which is an out-of-bounds access. On arm64, the `test_bit()` implementation uses the bit number to compute a word offset and bitmask. With a negative bit index, this results in accessing memory before the start of the `node_states` array, producing an immediate crash (typically a data abort or page fault during kernel execution).

The fundamental error is that the code assumed `max_nid` would always hold a valid node ID after the loop, but this is only true when the task has at least one non-zero NUMA fault entry. For tasks that have never been scanned by NUMA balancing (or whose fault counts have decayed to zero), `max_nid` remains at its sentinel value of `NUMA_NO_NODE`.

## Consequence

The consequence is a hard kernel crash (panic/oops) during boot on NUMA-enabled systems, particularly arm64 systems. The crash occurs when `task_numa_placement()` is called for a task with no accumulated NUMA fault statistics. The `node_state(-1, N_CPU)` call performs an out-of-bounds memory access on the `node_states` bitmap, which triggers:

- On arm64: a synchronous data abort (translation fault) leading to a kernel panic. Since this occurs during boot, the system fails to come up entirely.
- On x86_64: potentially a similar crash, though the timing and memory layout may differ. The out-of-bounds `test_bit(-1, ...)` accesses the word at offset `-sizeof(unsigned long)` from the bitmap, which could either crash immediately or silently read garbage (depending on what memory precedes the `node_states` array). If it reads a non-zero bit, the code enters the CPU-less node fallback path, which would then call `node_distance(NUMA_NO_NODE, nid)` — itself another invalid operation that could crash or return garbage.

The crash is deterministic: any arm64 system with NUMA enabled (`CONFIG_NUMA=y`, `CONFIG_NUMA_BALANCING=y`) that includes the buggy commit `5c7b1aaf139d` will crash during boot. Qian Cai confirmed that the fix resolves the boot crash, as indicated by the `Tested-by` tag on the fix commit.

## Fix Summary

The fix adds a single check for `NUMA_NO_NODE` before calling `node_state()`:

```c
/* Cannot migrate task to CPU-less node */
-	if (!node_state(max_nid, N_CPU)) {
+	if (max_nid != NUMA_NO_NODE && !node_state(max_nid, N_CPU)) {
```

This change ensures that `node_state()` is only called when `max_nid` holds a valid node index (i.e., not `-1`). When `max_nid` is `NUMA_NO_NODE`, the entire CPU-less node fallback block is skipped. This is correct because:

1. If `max_nid` is `NUMA_NO_NODE`, it means no node had any faults, so `max_faults` is also 0.
2. The code further down already handles the zero-faults case: `if (max_faults) { /* Set the new preferred node */ ... }` — when `max_faults` is 0, no preferred node update occurs.
3. Therefore, skipping the CPU-less node guard when `max_nid == NUMA_NO_NODE` is safe: the invalid sentinel value is never passed to `node_state()`, `node_distance()`, or `sched_setnuma()`, and the task's preferred node remains unchanged.

The fix is minimal, correct, and complete. It precisely guards against the one invalid code path without changing any other behavior of the NUMA placement logic.

## Triggering Conditions

The following conditions are required to trigger the crash:

- **Kernel configuration:** `CONFIG_NUMA=y` and `CONFIG_NUMA_BALANCING=y` must be enabled. These are standard on multi-socket server kernels for x86_64 and arm64.
- **Kernel version:** The kernel must contain commit `5c7b1aaf139d` ("sched/numa: Avoid migrating task to CPU-less node") but not the fix `ab31c7fd2d37`. Both commits were merged in the v5.18 development window, so only v5.18-rc1 kernels built between these two commits are affected.
- **NUMA topology:** The system must have NUMA enabled. The bug does not require CPU-less nodes — it triggers whenever a task has zero accumulated NUMA faults, which happens during early boot regardless of topology.
- **Architecture:** arm64 systems are confirmed to crash. x86_64 systems may also crash depending on memory layout around the `node_states` array.

The trigger condition is straightforward: `task_numa_placement()` is called for any task whose `numa_faults[]` array entries are all zero or have decayed to zero. During boot, newly forked tasks have all-zero fault arrays, so the very first invocation of `task_numa_placement()` for such a task will crash. The NUMA balancing scan is triggered by `task_tick_numa()` on each scheduler tick, which eventually schedules `task_numa_work()`, which calls `task_numa_placement()`. This happens automatically after the task has run for a sufficient number of ticks (controlled by `numa_balancing_scan_delay_ms`, default 1000ms).

There is no race condition involved. The bug is a deterministic logic error that triggers every time `task_numa_placement()` is called with no accumulated NUMA faults.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. The reasons are:

**1. task_numa_placement() requires userspace processes with mm_struct.** The function `task_numa_placement()` is only reachable through the NUMA balancing code path. It is called from `task_numa_work()`, which is registered as a `task_work` callback via `task_tick_numa()`. The `task_tick_numa()` function first checks `if (p->flags & PF_EXITING)` and `if (!p->mm)` (indirectly, through `curr_numa_group` and `p->numa_faults` checks). More critically, `task_numa_work()` accesses `p->mm` directly to iterate over VMAs for page scanning: `mm = p->mm; ... vma_iter_init(&vmi, mm, start);`. kSTEP tasks are kernel threads (created via `kstep_task_create()` or `kstep_kthread_create()`), which have `p->mm == NULL`. Kernel threads are never subject to NUMA balancing, so `task_numa_placement()` is never called for them.

**2. task_numa_placement() is a static function.** The function is declared `static void task_numa_placement(struct task_struct *p)` in `kernel/sched/fair.c`. It is not exported via `EXPORT_SYMBOL` and is not accessible through `kallsyms` in standard kernel builds (unless `CONFIG_KALLSYMS_ALL=y`). Even with `KSYM_IMPORT`, there is no guarantee the symbol would be resolvable, and calling it directly would require carefully setting up all the task's NUMA state (`numa_faults[]`, `numa_faults_locality[]`, `mm->numa_scan_seq`, etc.) to avoid crashing in other ways before reaching the buggy line.

**3. NUMA balancing requires real page fault handling.** The NUMA balancing mechanism works by periodically scanning a task's page table entries and marking them as `PROT_NONE` (protection-none). When the task subsequently accesses those pages, a NUMA hint page fault is generated, which records the fault in `p->numa_faults[]` and initiates potential page migration. This entire mechanism depends on real userspace memory access patterns, real page tables, and the virtual memory fault handling subsystem. kSTEP cannot simulate any of these.

**4. The crash occurs during early boot.** The specific crash reported by Qian Cai happened during boot, when early tasks triggered NUMA balancing for the first time. kSTEP loads as a kernel module after the system has fully booted, so boot-time crashes are inherently unreachable.

**5. What would need to change in kSTEP to support this?** Reproducing this bug would require one of:
   - Adding the ability to create real userspace processes with valid `mm_struct`, page tables, and mapped VMAs — this is a fundamental architectural change to kSTEP.
   - Adding a mechanism to directly invoke static kernel functions with arbitrary arguments — this would require either patching the kernel to export `task_numa_placement()` or using kprobes/ftrace to hook into it, neither of which is a minor kSTEP extension.
   - Adding a `kstep_task_set_numa_faults()` helper to pre-populate a task's `numa_faults[]` array and then somehow trigger the code path — but even this wouldn't work because the function requires a valid `mm->numa_scan_seq`.

**6. Alternative reproduction methods outside kSTEP:**
   - Build a kernel with only commit `5c7b1aaf139d` applied (or check out a tree at `ab31c7fd2d37~1`).
   - Boot it on an arm64 NUMA system (or a QEMU arm64 VM with NUMA topology configured via `-numa node` options).
   - The crash should occur automatically during boot when NUMA balancing first activates for any userspace task.
   - Alternatively, on x86_64, enable NUMA balancing (`echo 1 > /proc/sys/kernel/numa_balancing`) and run a workload — though the specific crash behavior may differ due to memory layout differences.
