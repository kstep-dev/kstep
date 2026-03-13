# Uclamp: Incomplete rq::uclamp Array Initialization

**Commit:** `dcd6dffb0a75741471297724640733fa4e958d72`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.6-rc1
**Buggy since:** v5.3-rc1 (introduced by commit 69842cba9ace "sched/uclamp: Add CPU's clamp buckets refcounting")

## Bug Description

The `init_uclamp()` function, which runs once during kernel boot to initialize the per-CPU utilization clamping data structures, incorrectly uses `sizeof(struct uclamp_rq)` to clear the `rq::uclamp` array. However, `rq::uclamp` is an array of `UCLAMP_CNT` (which equals 2) elements of type `struct uclamp_rq`, meaning it contains two entries: one for `UCLAMP_MIN` (index 0) and one for `UCLAMP_MAX` (index 1).

Because the `memset` call only specifies the size of a single `struct uclamp_rq`, only the first element of the array (`uclamp[UCLAMP_MIN]`) is zeroed. The second element (`uclamp[UCLAMP_MAX]`) is left in whatever state the per-CPU memory was in prior to initialization. This means the `value` field and the `bucket[]` array within `uclamp[UCLAMP_MAX]` are not explicitly cleared.

The `struct uclamp_rq` contains a `value` field (representing the aggregated clamp value for the CPU) and an array of `UCLAMP_BUCKETS` `struct uclamp_bucket` entries used for refcounting tasks in each utilization clamp bucket. If this structure is not properly zeroed, the CPU's maximum utilization clamp could start with an incorrect aggregated value or stale refcounts, potentially leading to incorrect CPU frequency selection or task placement decisions during early boot.

In practice, per-CPU data areas are typically zeroed by the kernel's early memory initialization, so the bug may not manifest as a runtime issue on most systems. However, the code is incorrect from a defensive programming standpoint: it relies on an implementation detail (pre-zeroed per-CPU memory) rather than explicitly initializing the full array.

## Root Cause

The root cause is a simple off-by-one-style size error in the `memset` call within `init_uclamp()` in `kernel/sched/core.c`. The buggy code is:

```c
for_each_possible_cpu(cpu) {
    memset(&cpu_rq(cpu)->uclamp, 0, sizeof(struct uclamp_rq));
    cpu_rq(cpu)->uclamp_flags = 0;
}
```

The `rq::uclamp` member is declared in `kernel/sched/sched.h` as:

```c
struct uclamp_rq  uclamp[UCLAMP_CNT] ____cacheline_aligned;
```

where `UCLAMP_CNT` is defined via the `enum uclamp_id` in `include/linux/sched.h`:

```c
enum uclamp_id {
    UCLAMP_MIN = 0,
    UCLAMP_MAX,
    UCLAMP_CNT   /* = 2 */
};
```

So `rq::uclamp` is an array of 2 `struct uclamp_rq` elements. The `memset` uses `sizeof(struct uclamp_rq)` which is the size of a single element, not the entire array. This means only `uclamp[0]` (i.e., `UCLAMP_MIN`) gets zeroed, while `uclamp[1]` (i.e., `UCLAMP_MAX`) is not touched by the `memset`.

Each `struct uclamp_rq` consists of:

```c
struct uclamp_rq {
    unsigned int value;
    struct uclamp_bucket bucket[UCLAMP_BUCKETS];
};
```

The `bucket[]` array holds per-bucket refcounts and values. If these are not zeroed, any subsequent enqueue/dequeue operations that manipulate `uclamp[UCLAMP_MAX]` could start from an inconsistent state. The `value` field determines the effective maximum utilization clamp for the CPU's runqueue, so a non-zero initial value could cause the CPU to be artificially clamped.

The introducing commit `69842cba9ace` added both the `memset` and a subsequent `for_each_clamp_id` loop that partially initializes specific bucket fields, but that loop does not substitute for zeroing the entire structure — it only sets `bucket[1].value` to `uclamp_none(clamp_id)` and does not reset all bucket refcounts or the `value` field for both array elements.

## Consequence

If the per-CPU memory backing `rq::uclamp[UCLAMP_MAX]` is not pre-zeroed, the consequences could include:

1. **Incorrect CPU frequency selection:** The `uclamp_rq_util_with()` function in `sched.h` reads `rq->uclamp[UCLAMP_MAX].value` to determine the maximum utilization clamp for a CPU. If this starts with a garbage value (e.g., a very low value), the CPU frequency governor (schedutil) could cap the CPU frequency too low, causing unexpected performance degradation during early boot tasks.

2. **Stale bucket refcounts:** The `bucket[]` array refcounts could start non-zero, meaning the uclamp aggregation logic in `uclamp_rq_inc()` / `uclamp_rq_dec()` would compute incorrect aggregated clamp values. This could cause tasks to be scheduled on CPUs that appear to have wrong capacity constraints, affecting task placement in energy-aware scheduling (EAS) environments.

3. **Silent data corruption:** Because the symptoms would manifest as suboptimal scheduling or frequency decisions rather than crashes, this bug could silently degrade system performance without any visible error messages, making it extremely difficult to diagnose.

In practice, most architectures zero per-CPU areas during early boot, so the bug is unlikely to cause visible symptoms. However, it represents a correctness issue that could become problematic if the kernel's memory initialization guarantees change or on architectures with different per-CPU allocation strategies.

## Fix Summary

The fix is a one-line change in `init_uclamp()` that corrects the `memset` size from `sizeof(struct uclamp_rq)` (size of one element) to `sizeof(struct uclamp_rq) * UCLAMP_CNT` (size of the entire array):

```c
/* Before (buggy): */
memset(&cpu_rq(cpu)->uclamp, 0, sizeof(struct uclamp_rq));

/* After (fixed): */
memset(&cpu_rq(cpu)->uclamp, 0, sizeof(struct uclamp_rq) * UCLAMP_CNT);
```

This ensures that both `uclamp[UCLAMP_MIN]` and `uclamp[UCLAMP_MAX]` are fully zeroed for every CPU's runqueue during initialization. The fix is correct and complete because `UCLAMP_CNT` is the exact dimension of the `uclamp[]` array, so `sizeof(struct uclamp_rq) * UCLAMP_CNT` covers the entire array.

An alternative correct fix would have been `sizeof(cpu_rq(cpu)->uclamp)` which would automatically track the array size if the dimension ever changed. However, the explicit multiplication by `UCLAMP_CNT` is equally correct and makes the intent clear.

## Triggering Conditions

- **Kernel configuration:** `CONFIG_UCLAMP_TASK=y` must be enabled (without it, `init_uclamp()` is a no-op stub).
- **Kernel version:** Must be between v5.3-rc1 (when `69842cba9ace` was merged) and v5.6-rc1 (when this fix was merged).
- **Memory state:** The per-CPU memory backing the `rq` structure must contain non-zero data at the offset of `rq::uclamp[1]` at the time `init_uclamp()` runs. In practice, this is unlikely on most systems because per-CPU areas are zeroed during early boot, but it is not guaranteed by the API.
- **Observation:** Even if the memory is not pre-zeroed, the bug would only be observable through indirect effects: incorrect CPU frequency scaling decisions, incorrect task placement by EAS, or unexpected utilization clamp values visible through tracing/debugging.
- **No race condition:** This is a purely sequential initialization bug in `__init` code that runs once during boot on a single CPU before any tasks are scheduled.
- **Probability:** Very low in practice due to per-CPU memory zeroing, but the code is definitively incorrect.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for two independent reasons:

### 1. Kernel Version Too Old (Pre-v5.15)

The bug exists in kernels from v5.3-rc1 to v5.6-rc1. kSTEP supports Linux v5.15 and newer only. The affected kernel versions (v5.3 through v5.5) are outside kSTEP's supported range. The fix commit `dcd6dffb0a75741471297724640733fa4e958d72` was merged for v5.6-rc1, which is also well before the v5.15 minimum.

### 2. Boot-Time __init Function Bug

Even if the kernel version were supported, this bug occurs in `init_uclamp()`, which is a `__init` function that runs once during kernel boot. By the time kSTEP loads as a kernel module:

- The `init_uclamp()` function has already completed and its code has been freed (marked `__init`).
- The `rq::uclamp` arrays have been in active use, with normal enqueue/dequeue operations having already written to `uclamp[UCLAMP_MAX]` fields.
- There is no way for kSTEP to "re-trigger" the initialization path or observe the initial uninitialized state.

### 3. Cannot Reset Per-CPU Initialization State

kSTEP cannot reset or re-initialize per-CPU `rq::uclamp` state to simulate the bug. The `init_uclamp()` function is a one-shot `__init` function, and there is no runtime API to re-run it. Manually writing garbage to `rq::uclamp[UCLAMP_MAX]` from a kSTEP driver would not be a valid reproduction — it would simulate the symptom, not the root cause.

### 4. No Observable Impact in Practice

Because per-CPU memory is zeroed during early boot on all supported architectures, the buggy `memset` in `init_uclamp()` is effectively a no-op correctness issue. Even on a v5.5 kernel, the `uclamp[UCLAMP_MAX]` fields would already be zero before `init_uclamp()` runs, making the bug impossible to observe through external behavior.

### 5. What Would Be Needed to Reproduce

To reproduce this bug outside kSTEP, one would need to:

1. Build a kernel in the v5.3–v5.5 range with `CONFIG_UCLAMP_TASK=y`.
2. Modify the kernel's per-CPU allocator to fill per-CPU areas with a known non-zero pattern (e.g., `0xCC`) instead of zeroing them, to make the uninitialized `uclamp[UCLAMP_MAX]` visible.
3. Boot the kernel and observe that `rq::uclamp[UCLAMP_MAX].value` starts at a non-zero value via ftrace or a custom debugfs interface.
4. Observe incorrect CPU frequency scaling or utilization clamping behavior during early boot tasks.

This approach requires modifying core kernel memory allocation, which is fundamentally outside kSTEP's capabilities.

### 6. Alternative Reproduction Methods

- **Static analysis:** The bug can be confirmed by code inspection alone — comparing `sizeof(struct uclamp_rq)` with the array dimension `UCLAMP_CNT`.
- **QEMU with modified kernel:** Boot a v5.5 kernel in QEMU with a patch to poison per-CPU memory, then check `rq::uclamp[UCLAMP_MAX]` values via `/proc/sched_debug` or ftrace.
- **Unit test approach:** Write a kernel patch that adds a `WARN_ON()` in `init_uclamp()` checking that `uclamp[UCLAMP_MAX]` fields are zero after the `memset`, which would fail on a poisoned-memory kernel.
