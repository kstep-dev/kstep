# Topology: Out-of-Bounds Read in hop_cmp() During NUMA CPU Lookup

**Commit:** `01bb11ad828b320749764fa93ad078db20d08a9e`
**Affected files:** kernel/sched/topology.c
**Fixed in:** v6.3-rc1
**Buggy since:** v6.3-rc1 (introduced by `cd7f55359c90` "sched: add sched_numa_find_nth_cpu()", which landed in the same merge window)

## Bug Description

The `sched_numa_find_nth_cpu()` function was added to the scheduler topology code to efficiently find the Nth CPU in a cpumask, ordered by NUMA distance from a given node. It uses a binary search (`bsearch()`) over the `sched_domains_numa_masks` array, where each level represents an increasing NUMA hop distance. The comparison function `hop_cmp()` is passed to `bsearch()` and is called for each candidate hop level during the search.

The bug is in the `hop_cmp()` comparison function. The function unconditionally initializes a `prev_hop` pointer by dereferencing the memory location one element before the current comparison element (`b - 1`). This dereference occurs even when the current element `b` is the first element of the `sched_domains_numa_masks` array, meaning `b - 1` points to memory before the array's start. This is an out-of-bounds memory read.

Although `prev_hop` is only *used* conditionally (when `b != k->masks`, i.e., not the first element), the C language does not guarantee that an unused dereference is safe — it is undefined behavior. KASAN (Kernel Address Sanitizer) detected this out-of-bounds access and flagged it as a warning. The bug was reported by Bruno Goncalves of Red Hat.

The function produces correct scheduling results regardless of the bug, because the garbage value read into `prev_hop` is never used when `b` points to the first element (the ternary expression `(b == k->masks) ? 0 : cpumask_weight_and(k->cpus, prev_hop[k->node])` short-circuits to 0 in that case). However, the unconditional dereference constitutes undefined behavior and could theoretically lead to unpredictable compiler optimizations or crashes on systems with strict memory protection.

## Root Cause

The root cause is in the `hop_cmp()` function in `kernel/sched/topology.c`. In the buggy version, the function begins with:

```c
static int hop_cmp(const void *a, const void *b)
{
    struct cpumask **prev_hop = *((struct cpumask ***)b - 1);
    struct cpumask **cur_hop = *(struct cpumask ***)b;
    struct __cmp_key *k = (struct __cmp_key *)a;

    if (cpumask_weight_and(k->cpus, cur_hop[k->node]) <= k->cpu)
        return 1;

    k->w = (b == k->masks) ? 0 : cpumask_weight_and(k->cpus, prev_hop[k->node]);
    if (k->w <= k->cpu)
        return 0;

    return -1;
}
```

The parameter `b` is a pointer into the `sched_domains_numa_masks` array (of type `struct cpumask ***`). The expression `*((struct cpumask ***)b - 1)` computes the address one element before `b` and dereferences it. When `bsearch()` compares the key against the first element of the array (index 0), `b` equals `k->masks` (the array base), and `b - 1` points to memory before the array.

The `sched_domains_numa_masks` array is allocated by `sched_init_numa()` during boot as a dynamically allocated array of `struct cpumask **` pointers. Reading one element before this allocation is an out-of-bounds heap access. KASAN instruments all heap accesses and detects when a read falls outside the allocated region, generating a warning.

The critical insight is that C does not guarantee "dead code elimination" of the dereference just because the result is discarded by the ternary. The dereference `*((struct cpumask ***)b - 1)` is a side effect that occurs at initialization time, before any conditional check. The compiler is free to keep this load instruction, and on architectures with strict memory protection or with KASAN enabled, it will trap.

The `bsearch()` function in `lib/bsearch.c` performs a standard binary search. For an array of `sched_domains_numa_levels` elements, it starts by comparing the middle element. If the searched-for CPU count is very small (e.g., cpu=0), the binary search will quickly narrow to the first element (index 0), triggering the out-of-bounds read. In fact, any call to `sched_numa_find_nth_cpu()` where `bsearch()` eventually examines the first element will trigger the bug.

## Consequence

The primary observable consequence is a KASAN warning in the kernel log. On systems running with `CONFIG_KASAN=y`, calling `sched_numa_find_nth_cpu()` (which is called by `cpumask_local_spread()`, used by network drivers like mlx5e and ice for IRQ affinity assignment) will produce a slab-out-of-bounds or similar KASAN report whenever the binary search examines the first hop level.

On systems without KASAN, the bug reads whatever memory happens to be adjacent to (before) the `sched_domains_numa_masks` array on the heap. Since the value is never actually used when `b == k->masks`, the function returns correct results. However, this constitutes undefined behavior in C:

1. A sufficiently aggressive compiler could theoretically optimize the ternary check away, reasoning that since `prev_hop` was already dereferenced (implying it is a valid pointer), the `b == k->masks` check must be false. This could lead to using garbage data for `k->w`, resulting in an incorrect CPU selection.
2. On architectures with hardware memory protection (e.g., guard pages), the read could trigger a page fault and kernel crash if the memory before the array happens to be unmapped.
3. In practice on common architectures (x86_64), the heap allocator typically places metadata or other allocations adjacent to the array, so the read succeeds but returns garbage — which is harmlessly discarded.

The bug does not cause incorrect scheduling behavior in practice. The `sched_numa_find_nth_cpu()` function returns the correct CPU on both buggy and fixed kernels. The only detectable symptom is the KASAN warning in kernel logs.

## Fix Summary

The fix reorganizes the `hop_cmp()` function to defer the dereference of `prev_hop` until after verifying that `b` does not point to the first element of the array. The fixed code:

```c
static int hop_cmp(const void *a, const void *b)
{
    struct cpumask **prev_hop, **cur_hop = *(struct cpumask ***)b;
    struct __cmp_key *k = (struct __cmp_key *)a;

    if (cpumask_weight_and(k->cpus, cur_hop[k->node]) <= k->cpu)
        return 1;

    if (b == k->masks) {
        k->w = 0;
        return 0;
    }

    prev_hop = *((struct cpumask ***)b - 1);
    k->w = cpumask_weight_and(k->cpus, prev_hop[k->node]);
    if (k->w <= k->cpu)
        return 0;

    return -1;
}
```

The key changes are: (1) `prev_hop` is declared but not initialized at the top of the function; (2) An explicit `if (b == k->masks)` check is performed *before* accessing `b - 1`, immediately returning 0 with `k->w = 0` when at the first hop; (3) Only after confirming `b != k->masks` is `prev_hop` initialized via `*((struct cpumask ***)b - 1)`.

This fix is correct because it preserves the exact same logic as the original: when `b` is the first element, `k->w` is set to 0 and the function returns 0 (meaning "found the right hop"); otherwise, `prev_hop` is safely computed from the previous array element. The fix eliminates the undefined behavior without changing the function's return values or the resulting CPU selection.

## Triggering Conditions

The bug triggers whenever `sched_numa_find_nth_cpu()` is called and `bsearch()` examines the first element (index 0) of `sched_domains_numa_masks`. The specific conditions are:

- **NUMA topology must be initialized**: The system must have NUMA topology information, meaning `sched_domains_numa_masks` is populated by `sched_init_numa()` during boot. This happens on any system with `CONFIG_NUMA=y` and at least one NUMA node.
- **A caller must invoke `sched_numa_find_nth_cpu()`**: The primary caller is `cpumask_local_spread()`, which is used by network drivers (mlx5e, ice, irdma, etc.) when assigning IRQ affinity masks. It is also called directly by `for_each_numa_hop_mask()`.
- **The binary search must visit index 0**: This happens when the requested CPU index is small (e.g., looking for the 0th or 1st CPU closest to a given node), which causes `bsearch()` to search toward the beginning of the array. On a system with few NUMA levels, nearly every call will visit index 0.
- **KASAN must be enabled to detect it**: `CONFIG_KASAN=y` is required to observe the warning. Without KASAN, the out-of-bounds read is silent and does not affect behavior.

The bug is deterministic and highly reproducible: any call to `sched_numa_find_nth_cpu()` that causes `bsearch()` to compare against the first element will trigger the out-of-bounds read. On a 2-node NUMA system (2 NUMA levels), the binary search always examines index 0, making the bug trigger on every single call.

## Reproduce Strategy (kSTEP)

This bug **cannot** be meaningfully reproduced with kSTEP, and is placed in `drivers_unplanned` for the following reasons:

### 1. Why this bug cannot be reproduced with kSTEP

The bug is a **memory safety violation** (out-of-bounds heap read), not a scheduling behavior bug. The `hop_cmp()` function produces identical, correct return values on both buggy and fixed kernels for all inputs. There is no scheduling behavior difference to detect — the same CPU is selected, tasks are scheduled identically, and all observable scheduling metrics (nr_running, curr_task, vruntime, etc.) are unchanged.

kSTEP's observation primitives (`kstep_pass()`, `kstep_fail()`, `kstep_output_curr_task()`, `kstep_output_nr_running()`, etc.) are designed to observe scheduling outcomes and state. They cannot detect out-of-bounds memory reads or other memory safety violations. Even if we call `sched_numa_find_nth_cpu()` from a kSTEP driver, the return value will be correct on both buggy and fixed kernels.

### 2. What would be needed to detect this in kSTEP

To detect this bug, the following would be required:

- **CONFIG_KASAN=y** in the kernel build configuration to instrument memory accesses and detect the out-of-bounds read.
- **A mechanism to parse kernel log output** for KASAN warnings (e.g., scanning `dmesg` for "BUG: KASAN: slab-out-of-bounds" or similar messages). kSTEP currently has no API for reading or filtering kernel log messages programmatically from a driver.
- **Adding a kSTEP API** like `kstep_check_kasan_report()` or `kstep_read_kmsg()` that could check whether a KASAN warning was emitted after a specific operation. This would be a FUNDAMENTAL change to kSTEP's architecture, which is built around observing scheduling state, not memory safety.

### 3. The bug is version-compatible but not behavior-observable

The bug exists in the v6.3-rc1 kernel cycle, which is within kSTEP's supported range (v5.15+). The affected code (`kernel/sched/topology.c`) is present and callable. However, the absence of any behavioral difference between buggy and fixed kernels makes a pass/fail determination impossible through scheduling observation alone.

### 4. Alternative reproduction methods

- **Build a kernel with CONFIG_KASAN=y** and boot it on any NUMA system (real or QEMU with `-numa` options). Load any network driver that calls `cpumask_local_spread()` (e.g., `modprobe mlx5_core` on appropriate hardware, or a simple module that calls `sched_numa_find_nth_cpu()` directly). Check `dmesg` for KASAN warnings.
- **Write a standalone kernel module** that calls `sched_numa_find_nth_cpu(cpu_online_mask, 0, 0)` and check the KASAN output.
- **Use a KASAN-enabled CI system** (like the Intel 0day robot that originally detected related issues in the patch series) to run tests that exercise `cpumask_local_spread()`.
- **Static analysis tools** (like Coverity or the compiler with `-fsanitize=undefined`) can also flag the unconditional dereference before the conditional use.
