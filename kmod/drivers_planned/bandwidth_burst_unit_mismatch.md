# Bandwidth: CFS Burst Parameter Unit Mismatch in cpu_max_write()

**Commit:** `49217ea147df7647cb89161b805c797487783fc0`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.10-rc1
**Buggy since:** v5.14-rc1 (commit f4183717b370 "sched/fair: Introduce the burstable CFS controller")

## Bug Description

In the Linux kernel's cgroup v2 CPU bandwidth controller, writing to the `cpu.max` control file to update the quota or period of a task group silently corrupts the burst parameter (`cpu.max.burst`). The bug is a unit conversion error: the `cpu_max_write()` function retrieves the current burst value in microseconds (via `tg_get_cfs_burst()`), but then passes it to `tg_set_cfs_bandwidth()` which expects the burst value in nanoseconds. This causes the internally stored burst to be divided by 1000 every time `cpu.max` is written.

The CFS bandwidth controller allows task groups to accumulate unused bandwidth (burst) to handle bursty workloads. The burst parameter is configured via `/sys/fs/cgroup/<group>/cpu.max.burst` (in microseconds for the user interface) and stored internally in `tg->cfs_bandwidth.burst` in nanoseconds. The `cpu.max` file controls quota and period, and the intent is that writing `cpu.max` should not alter the burst setting. However, due to this bug, every write to `cpu.max` divides the internal burst value by 1000 (NSEC_PER_USEC).

For example, if a user sets `cpu.max.burst` to 1,000,000 µs and then writes a new quota to `cpu.max`, the burst silently drops to 1,000 µs. A second write to `cpu.max` would further reduce it to 1 µs, and a third write would reduce it to 0. This makes the burst feature completely unreliable whenever the user changes quota or period through the cgroup v2 interface, which is a common operation in container orchestration systems.

The bug has been present since v5.14-rc1 when the burstable CFS controller was first introduced, and it affects all kernels up to and including v6.9. It was fixed in v6.10-rc1.

## Root Cause

The root cause is a unit mismatch between the return value of `tg_get_cfs_burst()` and the parameter expected by `tg_set_cfs_bandwidth()`.

The function `tg_get_cfs_burst()` (defined at approximately line 10964 of `kernel/sched/core.c` in the buggy version) reads the internal nanosecond value and converts it to microseconds for user-facing display:

```c
static long tg_get_cfs_burst(struct task_group *tg)
{
    u64 burst_us;
    burst_us = tg->cfs_bandwidth.burst;
    do_div(burst_us, NSEC_PER_USEC);  // converts ns → µs
    return burst_us;
}
```

The function `tg_set_cfs_bandwidth()` (line 10809) expects all its arguments (`period`, `quota`, `burst`) in nanoseconds, and stores them directly:

```c
static int tg_set_cfs_bandwidth(struct task_group *tg, u64 period, u64 quota,
                                u64 burst)
{
    ...
    cfs_b->burst = burst;  // stored as nanoseconds
    ...
}
```

In the buggy `cpu_max_write()` function (line 11400):

```c
static ssize_t cpu_max_write(struct kernfs_open_file *of,
                             char *buf, size_t nbytes, loff_t off)
{
    struct task_group *tg = css_tg(of_css(of));
    u64 period = tg_get_cfs_period(tg);     // returns µs
    u64 burst = tg_get_cfs_burst(tg);       // returns µs  ← BUG
    u64 quota;
    int ret;

    ret = cpu_period_quota_parse(buf, &period, &quota);  // converts period and quota: µs → ns
    if (!ret)
        ret = tg_set_cfs_bandwidth(tg, period, quota, burst);  // expects ns, gets µs for burst
    return ret ?: nbytes;
}
```

The `period` variable is also initially in microseconds (from `tg_get_cfs_period()`), but `cpu_period_quota_parse()` correctly converts it to nanoseconds by multiplying by `NSEC_PER_USEC` (line `*periodp *= NSEC_PER_USEC`). Similarly, the parsed `quota` is converted to nanoseconds within `cpu_period_quota_parse()`. However, the `burst` variable is not processed by `cpu_period_quota_parse()` — it is passed directly from `tg_get_cfs_burst()` (in microseconds) to `tg_set_cfs_bandwidth()` (which expects nanoseconds).

Interestingly, the other callers of `tg_set_cfs_bandwidth()` — namely `tg_set_cfs_quota()`, `tg_set_cfs_period()`, and `tg_set_cfs_burst()` — all correctly read `burst` directly from `tg->cfs_bandwidth.burst` (in nanoseconds) rather than going through `tg_get_cfs_burst()`. Only `cpu_max_write()` made this mistake, likely because it was written to mirror the pattern of using getter functions (`tg_get_cfs_period()`, `tg_get_cfs_burst()`) without considering the unit conversion that `cpu_period_quota_parse()` applies to `period` but not to `burst`.

## Consequence

The observable consequence is that the `cpu.max.burst` setting is silently corrupted every time the `cpu.max` file is written. Specifically, the burst value is divided by 1000 on each write. This means:

1. **First write to `cpu.max`:** Burst drops by a factor of 1000. For example, a burst of 1,000,000 µs (1 second) becomes 1,000 µs (1 millisecond).
2. **Second write to `cpu.max`:** Burst drops by another factor of 1000. The 1,000 µs becomes 1 µs.
3. **Third write to `cpu.max`:** Burst effectively becomes 0 (integer division truncation).

This has significant real-world impact for container workloads that use CFS bandwidth burst. In container orchestration systems like Kubernetes, cgroup parameters may be updated frequently (e.g., during resource limit changes, pod restarts, or cgroup reconfiguration). Each such update that modifies `cpu.max` silently destroys the burst configuration, causing previously bursty workloads to become throttled.

The throttling caused by the loss of burst capacity can lead to latency spikes, reduced throughput, and degraded application performance. Workloads that were designed to tolerate brief CPU bursts (e.g., web servers handling request spikes, batch processing jobs with variable CPU demand) would suddenly hit bandwidth limits without any explicit configuration change. Since the burst parameter silently changes without any error or warning, this bug is particularly insidious — administrators would see unexpected throttling with no obvious cause in the configuration. The bug does not cause a kernel crash, panic, or data corruption — it is a logical correctness issue that manifests as silent misconfiguration and subsequent performance degradation.

## Fix Summary

The fix is a single-line change in `cpu_max_write()` that replaces the call to `tg_get_cfs_burst(tg)` with a direct read of `tg->cfs_bandwidth.burst`:

```c
// Before (buggy):
u64 burst = tg_get_cfs_burst(tg);       // returns microseconds

// After (fixed):
u64 burst = tg->cfs_bandwidth.burst;    // reads nanoseconds directly
```

This aligns `cpu_max_write()` with the pattern used by `tg_set_cfs_quota()`, `tg_set_cfs_period()`, and `tg_set_cfs_burst()`, all of which read the burst value directly from `tg->cfs_bandwidth.burst` (in nanoseconds) when calling `tg_set_cfs_bandwidth()`.

The fix is correct because `tg->cfs_bandwidth.burst` stores the burst in nanoseconds, which is exactly the unit expected by `tg_set_cfs_bandwidth()`. By bypassing the `tg_get_cfs_burst()` getter (which converts to microseconds for user-facing display), the burst value is preserved correctly across writes to `cpu.max`. The fix is minimal and complete — there is no other location in the kernel where this unit mismatch occurs, and no other callers of `tg_set_cfs_bandwidth()` use the getter functions to retrieve values that should remain in nanoseconds.

## Triggering Conditions

The bug is triggered under the following precise conditions:

1. **Cgroup v2 with CFS bandwidth controller:** The kernel must be compiled with `CONFIG_CGROUPS`, `CONFIG_CGROUP_SCHED`, `CONFIG_FAIR_GROUP_SCHED`, and `CONFIG_CFS_BANDWIDTH` enabled. The system must be using cgroup v2 (unified hierarchy).

2. **Non-zero burst value:** The task group must have a non-zero `cpu.max.burst` setting. If burst is 0, the division by 1000 still produces 0, so no change is observed. The burst must be set to at least 1000 µs (1 ms) to see a non-trivial effect, since values less than 1000 µs would round down to 0 µs after the division.

3. **Write to `cpu.max`:** The user must write to the `cpu.max` file of the cgroup. This is the specific code path (`cpu_max_write()`) that contains the bug. Writing to `cpu.max.burst` alone does not trigger the bug. Writing to the cgroup v1 equivalents (`cpu.cfs_period_us`, `cpu.cfs_quota_us`) also does not trigger the bug, as those code paths (`tg_set_cfs_quota()`, `tg_set_cfs_period()`) correctly use `tg->cfs_bandwidth.burst` directly.

4. **Reproducibility:** The bug is 100% deterministic. There is no race condition or timing dependency. Every single write to `cpu.max` in the presence of a non-zero burst will corrupt the burst value. No special CPU count, topology, or workload is required — the bug is purely in the control plane path for configuring CFS bandwidth.

The sequence to trigger is straightforward:
- Create a cgroup (or use an existing non-root cgroup)
- Set `cpu.max.burst` to a value ≥ 1000 µs
- Write any valid value to `cpu.max` (e.g., the same quota and period that were already set)
- Read back `cpu.max.burst` — it will be the original value divided by 1000

## Reproduce Strategy (kSTEP)

This bug is straightforward to reproduce with kSTEP because it is a purely deterministic control-plane bug with no timing, workload, or hardware dependencies. The kSTEP framework already provides `kstep_cgroup_write()` which can write to arbitrary cgroup control files, and internal access to `tg->cfs_bandwidth.burst` for verification.

### Step-by-step driver plan:

1. **Create a cgroup:** Use `kstep_cgroup_create("test")` to create a test cgroup.

2. **Create and assign a task:** Create a CFS task with `kstep_task_create()` and add it to the cgroup with `kstep_cgroup_add_task("test", task->pid)`. A task must be present in the cgroup so that the cgroup's CPU controller is active.

3. **Set initial bandwidth and burst:** Write the initial quota and period to `cpu.max`:
   ```c
   kstep_cgroup_write("test", "cpu.max", "1000000 100000");
   ```
   This sets quota = 1,000,000 µs and period = 100,000 µs.

4. **Set the burst value:** Write the burst value to `cpu.max.burst`:
   ```c
   kstep_cgroup_write("test", "cpu.max.burst", "1000000");
   ```
   This sets burst = 1,000,000 µs. Internally, `tg->cfs_bandwidth.burst` should now be 1,000,000,000 ns (= 1,000,000 × 1000).

5. **Verify initial burst value:** Read `tg->cfs_bandwidth.burst` via internal access. Use `KSYM_IMPORT` if needed to resolve the task group from the cgroup, or traverse internal structures through `css_tg()`. The value should be 1,000,000,000 ns (10^9).
   ```c
   // Access through internal structures
   // e.g., find the task_group associated with the cgroup and read cfs_bandwidth.burst
   u64 burst_before = tg->cfs_bandwidth.burst;
   ```

6. **Trigger the bug:** Write to `cpu.max` again, changing only the quota:
   ```c
   kstep_cgroup_write("test", "cpu.max", "2000000 100000");
   ```
   This invokes `cpu_max_write()`, which in the buggy kernel will read `burst = tg_get_cfs_burst(tg)` returning 1,000,000 (µs), and then pass it to `tg_set_cfs_bandwidth()` which stores it as 1,000,000 (ns) — a factor of 1000 too small.

7. **Verify the bug:** Read `tg->cfs_bandwidth.burst` again. On the **buggy kernel**, the value should now be 1,000,000 ns (10^6) instead of 1,000,000,000 ns (10^9). That is, the burst dropped by a factor of 1000.
   ```c
   u64 burst_after = tg->cfs_bandwidth.burst;
   if (burst_after == burst_before) {
       kstep_pass("burst preserved: %llu ns", burst_after);
   } else {
       kstep_fail("burst corrupted: before=%llu ns, after=%llu ns (expected %llu ns)",
                  burst_before, burst_after, burst_before);
   }
   ```

8. **Pass/fail criteria:**
   - **Buggy kernel (pre-fix):** `burst_after` = 1,000,000 (ns), which is 1000× smaller than `burst_before` = 1,000,000,000 (ns). The driver should call `kstep_fail()`.
   - **Fixed kernel (post-fix):** `burst_after` = 1,000,000,000 (ns), equal to `burst_before`. The driver should call `kstep_pass()`.

### Implementation details:

- **Accessing the task group:** To read `tg->cfs_bandwidth.burst`, the driver needs to find the `struct task_group` associated with the cgroup. This can be done by accessing the task's `sched_task_group` field (via `task->sched_task_group`) after the task has been added to the cgroup, or by using `css_tg()` on the cgroup's css. kSTEP's internal.h provides access to `sched.h` internals including `struct task_group` and `struct cfs_bandwidth`.

- **No special topology needed:** The bug does not depend on CPU count, topology, or NUMA configuration. A minimal 2-CPU QEMU setup is sufficient.

- **No tasks need to run:** The bug is in the control-plane path, not the scheduling path. The task only needs to exist in the cgroup to keep the CPU controller active; it does not need to actually run or consume CPU. Using `kstep_task_pause()` after creating the task is fine.

- **Kernel version guard:** Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0)` since the burstable CFS controller was introduced in v5.14-rc1. The buggy code path exists from v5.14 through v6.9.

- **Demonstrating external impact (optional):** Beyond checking internal state, one could also demonstrate the practical impact by creating a bandwidth-throttled workload where the burst was supposed to prevent throttling, showing that after `cpu.max` is rewritten, the task starts getting throttled due to the loss of burst capacity. However, the internal state check is sufficient and more deterministic.
