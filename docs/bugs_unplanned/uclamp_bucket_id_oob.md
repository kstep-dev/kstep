# Uclamp: Out-of-Bound Bucket ID Access

**Commit:** `6d2f8909a5fabb73fe2a63918117943986c39b6c`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.13-rc1
**Buggy since:** v5.3-rc1 (commit 69842cba9ace "sched/uclamp: Add CPU's clamp buckets refcounting")

## Bug Description

The Linux scheduler's utilization clamping (uclamp) subsystem organizes tasks into buckets based on their clamp values for efficient tracking. Each CPU's run queue maintains per-clamp-type arrays of buckets (`rq->uclamp[clamp_id].bucket[UCLAMP_BUCKETS]`), where `UCLAMP_BUCKETS` is a compile-time constant configurable between 5 and 20 via `CONFIG_UCLAMP_BUCKETS_COUNT`. The function `uclamp_bucket_id()` maps a task's clamp value (range [0, 1024] where 1024 is `SCHED_CAPACITY_SCALE`) to a bucket index.

The bucket size `UCLAMP_BUCKET_DELTA` is computed using `DIV_ROUND_CLOSEST(SCHED_CAPACITY_SCALE, UCLAMP_BUCKETS)`, which performs a rounding division. For certain values of `UCLAMP_BUCKETS`, this rounding division produces a bucket delta that does not evenly cover the full range [0, 1024], causing the maximum clamp value (1024) to map to a bucket index equal to `UCLAMP_BUCKETS` — one past the end of the valid range [0, UCLAMP_BUCKETS-1].

For example, with the maximum configuration of 20 buckets: `UCLAMP_BUCKET_DELTA = DIV_ROUND_CLOSEST(1024, 20) = 51`. A task with `clamp_value = 1024` (the maximum, i.e., `SCHED_CAPACITY_SCALE`) produces `bucket_id = 1024 / 51 = 20`, which is out of bounds since valid indices are [0, 19]. This arithmetic overflow affects many common `UCLAMP_BUCKETS_COUNT` values including 7, 8, 10, 11, 12, 14, 15, 16, 17, and 20.

## Root Cause

The root cause is in the `uclamp_bucket_id()` function in `kernel/sched/core.c`:

```c
#define UCLAMP_BUCKET_DELTA DIV_ROUND_CLOSEST(SCHED_CAPACITY_SCALE, UCLAMP_BUCKETS)

static inline unsigned int uclamp_bucket_id(unsigned int clamp_value)
{
    return clamp_value / UCLAMP_BUCKET_DELTA;
}
```

`SCHED_CAPACITY_SCALE` is 1024. `DIV_ROUND_CLOSEST(a, b)` computes `(a + b/2) / b`, which rounds to the nearest integer. For `UCLAMP_BUCKETS = 20`, this yields `(1024 + 10) / 20 = 51` (rounding up from 51.2). However, `20 * 51 = 1020`, not 1024. This means the last bucket (index 19) covers values up to `19 * 51 + 50 = 1019`, but clamp values can reach 1024. The value 1024 divided by 51 yields 20, which is one past the last valid bucket index.

The mismatch arises because rounding division produces a bucket delta whose product with `UCLAMP_BUCKETS` does not equal `SCHED_CAPACITY_SCALE`. When `UCLAMP_BUCKETS * UCLAMP_BUCKET_DELTA < SCHED_CAPACITY_SCALE`, the last few clamp values spill into a non-existent bucket. The specific bucket count values that trigger this depend on the rounding behavior: values where `DIV_ROUND_CLOSEST` rounds up (making the delta larger) are safe, while values where it rounds down or stays exact but doesn't perfectly tile are vulnerable.

Dietmar Eggemann noted in the review thread that across the configurable range [5, 20], the error in bucket count (difference between the computed bucket ID for value 1024 and the maximum valid index) ranges from -2 to +5, meaning some configurations produce bucket IDs up to 5 past the end of the array.

The function `uclamp_bucket_id()` is called from `uclamp_se_set()`, which in turn is invoked from multiple paths: `__setscheduler_uclamp()` (during `sched_setattr()`), `uclamp_update_util_min_rt_default()` (for RT tasks when the sysctl changes), `sysctl_sched_uclamp_handler()` (when writing `sched_uclamp_util_min` or `sched_uclamp_util_max`), and `cpu_util_update_eff()` (during cgroup effective clamp computation). Any of these paths can supply `SCHED_CAPACITY_SCALE` (1024) as the clamp value, triggering the out-of-bounds access.

## Consequence

The out-of-bounds bucket index is stored in `uc_se->bucket_id` and subsequently used to index into `rq->uclamp[clamp_id].bucket[]` during task enqueue (`uclamp_rq_inc_id()`) and dequeue (`uclamp_rq_dec_id()`). These functions increment and decrement the `tasks` counter and update the `value` field of the indexed bucket. With an OOB index, these operations corrupt memory adjacent to the bucket array in the `struct uclamp_rq`, which is embedded in the per-CPU `struct rq`.

The corruption targets whatever data structure follows the `bucket[UCLAMP_BUCKETS]` array in `struct uclamp_rq`. Depending on struct layout and padding, this could corrupt other run queue fields, leading to unpredictable scheduling behavior, incorrect task accounting, or kernel panics. With KASAN (Kernel Address Sanitizer) enabled, this triggers a slab-out-of-bounds error report.

In the worst case, corrupted run queue state can lead to scheduler lockups, priority inversions, or use-after-free conditions when corrupted pointers are followed. Even without an immediate crash, the silent corruption of adjacent fields means the system operates with incorrect scheduling metadata, potentially causing subtle performance degradation or fairness violations that are extremely difficult to diagnose.

## Fix Summary

The fix adds a bounds clamp to the return value of `uclamp_bucket_id()`:

```c
static inline unsigned int uclamp_bucket_id(unsigned int clamp_value)
{
    return min_t(unsigned int, clamp_value / UCLAMP_BUCKET_DELTA, UCLAMP_BUCKETS - 1);
}
```

The `min_t(unsigned int, ..., UCLAMP_BUCKETS - 1)` ensures the returned bucket ID never exceeds the maximum valid index, regardless of the `UCLAMP_BUCKET_DELTA` rounding behavior. Tasks with clamp values near the top of the range are placed into the last bucket (index `UCLAMP_BUCKETS - 1`), which is the semantically correct behavior since the last bucket should collect all values at or above its lower bound.

The fix deliberately retains the `DIV_ROUND_CLOSEST` rounding division for `UCLAMP_BUCKET_DELTA` rather than switching to a floor division. As Vincent Guittot noted in the v3 review, the rounding division provides better fairness in bucket size distribution across the range, and the `min_t` clamp handles the edge case cleanly. This was the result of iteration from v1 and v2 of the patch where alternative approaches were considered.

## Triggering Conditions

- **Kernel configuration:** `CONFIG_UCLAMP_TASK=y` with `CONFIG_UCLAMP_BUCKETS_COUNT` set to a value that causes the overflow. Affected values include 7, 8, 10, 11, 12, 14, 15, 16, 17, and 20. The default value of 5 does NOT trigger the bug.
- **Number of CPUs:** Any (the bug is per-CPU but the same arithmetic applies to all CPUs).
- **Trigger action:** Any operation that calls `uclamp_se_set()` with `value = SCHED_CAPACITY_SCALE` (1024). This includes:
  - Setting `sched_uclamp_util_min` or `sched_uclamp_util_max` sysctl to 1024 via procfs.
  - Using `sched_setattr()` syscall to set a task's uclamp min/max to 1024.
  - Cgroup `cpu.uclamp.min` or `cpu.uclamp.max` set to "max" (which maps to 1024).
  - Creating an RT task when `sysctl_sched_uclamp_util_min_rt_default` is 1024 (its default value).
- **Timing:** No race condition required. The bug triggers deterministically on the first call to `uclamp_bucket_id(1024)` with an affected bucket count configuration.
- **Probability:** 100% reproducible on affected configurations. The bug is a pure arithmetic error with no timing dependency.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The fix commit `6d2f8909a5fabb73fe2a63918117943986c39b6c` is tagged at v5.12 and was merged into v5.13-rc1. kSTEP requires Linux v5.15 or newer. The parent commit (`6d2f8909a~1`) is based on a kernel between v5.12 and v5.13-rc1, which is before kSTEP's minimum supported version of v5.15. Since the bug was already fixed before v5.15, there is no kernel version within kSTEP's supported range that contains this bug. Checking out the parent commit to create a buggy kernel would produce a pre-v5.13 kernel that kSTEP's build system and driver infrastructure cannot support.

2. **WHAT would need to be added to kSTEP to support this?**
   The fundamental limitation is kernel version compatibility, not missing kSTEP APIs. If kSTEP supported v5.12 kernels, the bug would be straightforward to reproduce: configure the kernel with `CONFIG_UCLAMP_BUCKETS_COUNT=20`, use `kstep_sysctl_write()` to set `sched_uclamp_util_min` to 1024, create an RT task, and verify that the task's `uclamp_req[UCLAMP_MIN].bucket_id` exceeds `UCLAMP_BUCKETS - 1`. Alternatively, one could observe memory corruption via KASAN or by reading adjacent fields before and after the uclamp set operation.

3. **Version status:** The fix is in v5.13-rc1 (pre-v5.15). kSTEP supports v5.15+ only. All v5.15+ kernels already include this fix.

4. **Alternative reproduction methods outside kSTEP:**
   - Build a v5.12 kernel with `CONFIG_UCLAMP_BUCKETS_COUNT=20` and `CONFIG_KASAN=y`.
   - Boot the kernel and write 1024 to `/proc/sys/kernel/sched_uclamp_util_min`.
   - Alternatively, use `sched_setattr()` to set a task's `sched_util_max = 1024`.
   - KASAN will immediately report a slab-out-of-bounds write in `uclamp_rq_inc_id()` when the task is enqueued.
   - Without KASAN, one can verify the bug by adding a `WARN_ON(bucket_id >= UCLAMP_BUCKETS)` check to `uclamp_bucket_id()` before the fix is applied.
