# Uclamp: Negative Value Bypass in cpu_uclamp_write()

**Commit:** `b562d140649966d4daedd0483a8fe59ad3bb465a`
**Affected files:** `kernel/sched/core.c`
**Fixed in:** v5.6-rc2
**Buggy since:** v5.3-rc1 (commit `2480c093130f` "sched/uclamp: Extend CPU's cgroup controller")

## Bug Description

The utilization clamping (uclamp) cgroup interface allows users to set minimum and maximum utilization values for tasks within a cgroup via the `cpu.uclamp.min` and `cpu.uclamp.max` files. These values are expressed as percentages in the range [0, 100] (with two decimal places of precision, i.e., internally scaled by 100 to give an integer range of [0, 10000]). The `capacity_from_percent()` function parses the user-provided string, validates it, and converts it to an internal utilization value.

A signed/unsigned comparison mismatch in `capacity_from_percent()` allowed negative values to bypass the range validation check. When a user wrote a negative value (e.g., `echo -1 > cpu.uclamp.min`), the function `cgroup_parse_float()` would parse it and store the result in `req.percent`, which is of type `s64` (signed 64-bit integer). The subsequent comparison `req.percent > UCLAMP_PERCENT_SCALE` used signed comparison semantics, where any negative `s64` value is always less than the positive `UCLAMP_PERCENT_SCALE` constant (10000). Therefore, the negative value passed the range check.

The negative `s64` value was then used in arithmetic that treated it as a large positive number when assigned to `req.util` (a `u64`), resulting in absurdly large utilization values being stored in the task group's uclamp request. Reading back the value via `cat cpu.uclamp.min` would show a garbage percentage like `42949671.96`, corresponding to the two's complement reinterpretation of the negative value as an unsigned integer.

This bug affected all kernels from v5.3-rc1 (when the uclamp cgroup controller was introduced) through v5.6-rc1.

## Root Cause

The root cause is a type mismatch in the comparison within `capacity_from_percent()` in `kernel/sched/core.c`. The `uclamp_request` structure defines `percent` as `s64`:

```c
struct uclamp_request {
    s64 percent;
    u64 util;
    int ret;
};
```

The function `cgroup_parse_float()` parses the user input string and stores the result in `req.percent`. When a negative number like `-1` is written, `cgroup_parse_float()` succeeds and stores `-100` (i.e., -1.00 scaled by 10^2) in `req.percent`.

The critical buggy line is:

```c
if (req.percent > UCLAMP_PERCENT_SCALE) {
    req.ret = -ERANGE;
    return req;
}
```

Here, `UCLAMP_PERCENT_SCALE` is defined as `(100 * POW10(UCLAMP_PERCENT_SHIFT))` where `UCLAMP_PERCENT_SHIFT` is 2, yielding `100 * 100 = 10000`. This evaluates to an `unsigned int` value of 10000. However, because `req.percent` is `s64`, the C language's implicit conversion rules promote the `unsigned int` on the right-hand side to `s64` for the comparison. In signed arithmetic, `-100 > 10000` is false, so the check passes.

After the bogus validation, the code proceeds to compute:

```c
req.util = req.percent << SCHED_CAPACITY_SHIFT;
req.util = DIV_ROUND_CLOSEST_ULL(req.util, UCLAMP_PERCENT_SCALE);
```

Since `req.util` is `u64`, the negative `s64` value stored in `req.percent` is reinterpreted as a very large unsigned value during the left shift and division. For input `-1`, `cgroup_parse_float()` stores `-100` in `req.percent`. When cast to `u64`, this becomes `0xFFFFFFFFFFFFFF9C`, which after shifting and dividing produces a nonsensical utilization value far exceeding `SCHED_CAPACITY_SCALE` (1024).

The fundamental error is that the comparison was intended to reject out-of-range values (both negative and >100%), but the signed comparison only caught positive out-of-range values, leaving negative values completely unchecked.

## Consequence

The immediate observable consequence is that writing a negative value to `cpu.uclamp.min` or `cpu.uclamp.max` succeeds instead of returning `-ERANGE`. The stored utilization value becomes an absurdly large number, and reading the cgroup file back shows a wildly incorrect percentage (e.g., `42949671.96`).

The corrupted uclamp value propagates through the scheduler's utilization clamping machinery. When `cpu_util_update_eff()` computes the effective uclamp values for a cgroup hierarchy, the bogus value can override legitimate uclamp constraints. For CFS tasks in the affected cgroup, the scheduler's frequency selection (via schedutil) would see an enormously inflated minimum or maximum utilization, potentially driving CPUs to maximum frequency unnecessarily (for `uclamp.min`) or allowing unrestricted frequency boosting (for `uclamp.max` with a huge value that effectively disables any ceiling). This wastes power on battery-constrained devices and undermines the purpose of the uclamp mechanism.

On asymmetric capacity systems (e.g., Arm big.LITTLE), corrupted uclamp values also affect task placement decisions. Functions like `uclamp_rq_util_with()` and `task_fits_capacity()` rely on uclamp values to determine whether a task fits on a particular CPU. An absurdly large `uclamp.min` could force tasks onto big cores unnecessarily, or an incorrect `uclamp.max` could affect capacity-aware wake-up decisions. While this does not cause a kernel crash, it results in incorrect scheduling behavior and potential performance degradation or excessive power consumption.

## Fix Summary

The fix is a single-character change that casts `req.percent` to `u64` before the comparison with `UCLAMP_PERCENT_SCALE`:

```c
-		if (req.percent > UCLAMP_PERCENT_SCALE) {
+		if ((u64)req.percent > UCLAMP_PERCENT_SCALE) {
```

By casting `req.percent` (which is `s64`) to `u64`, any negative value stored in `req.percent` becomes a very large positive unsigned value (due to two's complement representation). For example, `-100` as `s64` becomes `18446744073709551516` as `u64`, which is vastly greater than `UCLAMP_PERCENT_SCALE` (10000). This ensures the `> UCLAMP_PERCENT_SCALE` comparison correctly triggers for negative inputs, causing the function to set `req.ret = -ERANGE` and return early.

The fix is correct and complete because the only entry point for the bug is the comparison in `capacity_from_percent()`. After the fix, the function correctly rejects any input that parses to a negative value (returning `-ERANGE` to userspace as "Numerical result out of range"), while still accepting valid values in the range [0, 100.00]. The downstream arithmetic on `req.util` is never reached for invalid inputs, so no further changes are needed.

## Triggering Conditions

The bug is trivially triggered by writing any negative numeric value to the `cpu.uclamp.min` or `cpu.uclamp.max` cgroup control files. The specific steps are:

1. **Kernel configuration**: `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y` must be enabled. The cgroup v2 CPU controller must be available and mounted.
2. **Cgroup setup**: A cgroup hierarchy with the CPU controller enabled must exist. The default root cgroup suffices.
3. **Trigger**: Write any negative value to the uclamp control file, e.g., `echo -1 > /sys/fs/cgroup/<group>/cpu.uclamp.min`.
4. **Verification**: Read back the value with `cat /sys/fs/cgroup/<group>/cpu.uclamp.min` — it will show a garbage value like `42949671.96` instead of rejecting the write.

There are no race conditions, no timing dependencies, and no special topology or workload requirements. The bug is 100% deterministic and triggers on every attempt with any negative input value. Any user or process with write access to the cgroup's uclamp files can trigger it.

The bug affects kernels from v5.3-rc1 through v5.6-rc1. It requires the `CONFIG_UCLAMP_TASK_GROUP` option, which was relatively new at the time and not enabled in all distribution kernels.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP because the fix targets kernel v5.6-rc2, which is older than the minimum supported kernel version of v5.15.

1. **Why it cannot be reproduced**: kSTEP supports Linux v5.15 and newer only. The bug was introduced in v5.3-rc1 (commit `2480c093130f`) and fixed in v5.6-rc2 (commit `b562d140649966d4daedd0483a8fe59ad3bb465a`). By v5.15, the fix has been present for over a year and a half. There is no kernel version within kSTEP's supported range where the buggy code exists. Checking out a pre-fix commit would produce a kernel far too old for the kSTEP framework to build and run correctly.

2. **What would need to be added to kSTEP**: Even if the version constraint were relaxed, reproducing this bug would require writing to a cgroup control file (`cpu.uclamp.min` or `cpu.uclamp.max`) with a negative value. kSTEP currently provides `kstep_cgroup_set_cpuset()` and `kstep_cgroup_set_weight()`, but no API for setting uclamp values on a cgroup. A `kstep_cgroup_set_uclamp_min(name, value)` / `kstep_cgroup_set_uclamp_max(name, value)` function would be needed. Alternatively, since the bug is in the parsing layer (`capacity_from_percent()`), one could add a generic `kstep_cgroup_write(name, file, buf)` that writes an arbitrary string to a cgroup control file. However, none of this matters given the version constraint.

3. **Version constraint**: This is a "KERNEL VERSION TOO OLD" case. The fix was merged into v5.6-rc2, well before kSTEP's minimum supported version of v5.15.

4. **Alternative reproduction methods**: The bug is trivially reproducible outside kSTEP on any kernel between v5.3-rc1 and v5.6-rc1 with `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y`:
   - Boot a v5.5 kernel with uclamp support enabled.
   - Mount cgroup v2: `mount -t cgroup2 none /sys/fs/cgroup`.
   - Enable the CPU controller: `echo +cpu > /sys/fs/cgroup/cgroup.subtree_control`.
   - Create a test cgroup: `mkdir /sys/fs/cgroup/test`.
   - Write a negative value: `echo -1 > /sys/fs/cgroup/test/cpu.uclamp.min`.
   - Read back: `cat /sys/fs/cgroup/test/cpu.uclamp.min` — expect `42949671.96`.
   - On the fixed kernel (v5.6-rc2+), the `echo -1` command fails with "Numerical result out of range".
   
   This can also be tested in QEMU with a custom-built kernel, without any special hardware requirements.
