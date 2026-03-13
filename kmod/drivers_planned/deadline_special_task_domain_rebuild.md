# Deadline: Special Task Bandwidth Inflation During Domain Rebuild

**Commit:** `f6147af176eaa4027b692fdbb1a0a60dfaa1e9b6`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.15-rc1
**Buggy since:** v6.14-rc1 (introduced by commit `53916d5fd3c0` "sched/deadline: Check bandwidth overflow earlier for hotplug")

## Bug Description

The SCHED_DEADLINE scheduling class supports "special" tasks — specifically the schedutil frequency governor kthreads (sugov kthreads). These tasks are marked with the `SCHED_FLAG_SUGOV` flag and are assigned a fake SCHED_DEADLINE bandwidth (1ms runtime / 10ms period) solely to ensure that sleeping and priority inheritance mechanisms work correctly for them. This fake bandwidth is intentionally excluded from normal DEADLINE admission control and runtime enforcement: the kernel does not count sugov bandwidth against the root domain's `dl_bw.total_bw` during task scheduling parameter changes (see `dl_task_check_affinity` and `__checkparam_dl` which both early-return for `SCHED_FLAG_SUGOV` tasks).

However, when scheduling domains are rebuilt — which happens during CPU suspend/resume, CPU hotplug, or cpuset reconfiguration — the function `dl_rebuild_rd_accounting()` iterates over all tasks in each cpuset and calls `dl_add_task_root_domain()` for each. This function only checked `if (!dl_task(p))` to skip non-deadline tasks but did not filter out special DEADLINE tasks (sugov kthreads). Since sugov kthreads ARE `dl_task()` (they have `SCHED_DEADLINE` policy), their fake bandwidth was incorrectly added to the root domain's `total_bw`.

This inflated `total_bw` caused a cascade failure: when the next CPU was being taken offline (e.g., during suspend), `dl_bw_deactivate()` would check whether the remaining CPUs had enough capacity to handle the DEADLINE bandwidth. With the falsely inflated `total_bw` (which now included the fake sugov bandwidth), the overflow check would fail, returning `-EBUSY` (-16). This prevented CPUs from going offline, causing the entire suspend operation to fail.

The bug was reported by Jon Hunter on an NVIDIA Tegra186 board configured to boot with `isolcpus`. The board would fail to suspend with the error message: `Error taking CPU1 down: -16` / `Non-boot CPUs are not disabled`.

## Root Cause

The root cause lies in the `dl_add_task_root_domain()` function in `kernel/sched/deadline.c`. Before the fix, the function's guard clause was:

```c
void dl_add_task_root_domain(struct task_struct *p)
{
    ...
    raw_spin_lock_irqsave(&p->pi_lock, rf.flags);
    if (!dl_task(p)) {
        raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
        return;
    }
    ...
    __dl_add(dl_b, p->dl.dl_bw, cpumask_weight(rq->rd->span));
    ...
}
```

The function checked only whether the task was a DEADLINE task (`dl_task(p)`). If yes, it proceeded to add the task's `dl_bw` to the root domain's `total_bw` via `__dl_add()`. It did not distinguish between regular DEADLINE tasks and "special" DEADLINE tasks (sugov kthreads).

A DEADLINE task is considered "special" if its `dl_se->flags` has the `SCHED_FLAG_SUGOV` bit set. The `dl_entity_is_special()` function in `kernel/sched/sched.h` implements this check:

```c
static inline bool dl_entity_is_special(const struct sched_dl_entity *dl_se)
{
#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
    return unlikely(dl_se->flags & SCHED_FLAG_SUGOV);
#else
    return false;
#endif
}
```

Sugov kthreads are created in `kernel/sched/cpufreq_schedutil.c` in `sugov_kthread_create()` with explicitly fake bandwidth parameters:

```c
struct sched_attr attr = {
    .size           = sizeof(struct sched_attr),
    .sched_policy   = SCHED_DEADLINE,
    .sched_flags    = SCHED_FLAG_SUGOV,
    .sched_runtime  = NSEC_PER_MSEC,        /* 1ms - fake */
    .sched_deadline = 10 * NSEC_PER_MSEC,   /* 10ms - fake */
    .sched_period   = 10 * NSEC_PER_MSEC,   /* 10ms - fake */
};
```

The comment in the code explicitly says "Fake (unused) bandwidth; workaround to 'fix' priority inheritance." This fake bandwidth equates to approximately 10% utilization per sugov kthread. On a system with multiple sugov kthreads (one per cpufreq policy, potentially one per CPU), this fake bandwidth accumulates significantly.

The call chain that triggers the incorrect accounting is: `partition_sched_domains_locked()` → `dl_rebuild_rd_accounting()` → `dl_update_tasks_root_domain()` (for each cpuset) → `dl_add_task_root_domain()` (for each task). In `dl_rebuild_rd_accounting()` (in `kernel/cgroup/cpuset.c`), the function first clears all root domain bandwidth via `dl_clear_root_domain_cpu()`, then re-adds task bandwidth. The clearing step correctly handles dl_server bandwidth (from fair_server, ext_server), but the re-adding step fails to exclude special tasks.

The bug was exposed by commit `53916d5fd3c0` which moved the `dl_bw_deactivate()` call to the very beginning of `sched_cpu_deactivate()`. Before that commit, the bandwidth overflow check happened later (inside `cpuset_cpu_inactive()`), and the race window was different. With the earlier check positioning, the inflated `total_bw` from a previous domain rebuild directly caused the deactivation to fail.

## Consequence

The primary observable consequence is a **complete failure of CPU suspend/resume**. When the system attempts to suspend (e.g., via `echo mem > /sys/power/state`), the kernel tries to offline all non-boot CPUs. The `sched_cpu_deactivate()` function is called for each CPU. Due to the inflated `total_bw`, the `dl_bw_deactivate()` check perceives a bandwidth overflow and returns `-EBUSY` (error code -16). The CPU hotplug framework then reports:

```
Error taking CPU1 down: -16
Non-boot CPUs are not disabled
```

This means the suspend operation is aborted entirely. The system remains awake and cannot enter any sleep state (S3/suspend-to-RAM, etc.). This is a critical regression for embedded and mobile platforms where suspend/resume is essential for power management. The bug was specifically reported on NVIDIA Tegra186 (ARM64) boards running with `isolcpus` boot parameter, but it affects any system with:
1. `CONFIG_CPU_FREQ_GOV_SCHEDUTIL` enabled (creating sugov kthreads)
2. Scheduling domain rebuilds occurring (suspend, cpuset changes, CPU hotplug)
3. Subsequent CPU offline operations that check DEADLINE bandwidth

Beyond suspend failure, the inflated `total_bw` could also cause false admission control failures for legitimate DEADLINE tasks. If a user tries to set a task to SCHED_DEADLINE after a domain rebuild, the admission check via `__dl_overflow()` would see less available bandwidth than actually exists, potentially rejecting valid DEADLINE task configurations.

## Fix Summary

The fix is a single-line change in `dl_add_task_root_domain()` that adds the `dl_entity_is_special()` check to the guard clause:

```c
- if (!dl_task(p)) {
+ if (!dl_task(p) || dl_entity_is_special(&p->dl)) {
      raw_spin_unlock_irqrestore(&p->pi_lock, rf.flags);
      return;
  }
```

This ensures that when `dl_rebuild_rd_accounting()` iterates over all tasks during a domain rebuild, sugov kthreads (and any other "special" DEADLINE entities) are skipped. Their fake bandwidth is not added to the root domain's `total_bw`. This is consistent with how special tasks are handled everywhere else in the DEADLINE code: `dl_task_check_affinity()` returns early for `SCHED_FLAG_SUGOV`, `__checkparam_dl()` returns early for `SCHED_FLAG_SUGOV`, and `dl_change_utilization()` has a `WARN_ON_ONCE` if called for sugov tasks.

The fix is correct and complete because special DEADLINE tasks by definition do not consume real bandwidth. Their fake parameters exist solely for the priority inheritance mechanism (PI boosting). The sugov kthreads' "bandwidth" is never enforced by the DEADLINE runtime accounting — `update_dl_entity()` and the replenishment timer skip enforcement for special entities. Therefore, including their bandwidth in `total_bw` during domain rebuilds is always wrong, regardless of the specific trigger (suspend, cpuset change, or hotplug).

## Triggering Conditions

The bug requires the following conditions to be met simultaneously:

1. **Special DEADLINE tasks must exist**: The kernel must be compiled with `CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y`, and the schedutil cpufreq governor must be active, creating sugov kthreads. Alternatively, any task that has been set to SCHED_DEADLINE with the `SCHED_FLAG_SUGOV` flag via `sched_setattr_nocheck()` will trigger the bug. The number of sugov kthreads depends on the cpufreq driver and topology — typically one kthread per cpufreq policy.

2. **A scheduling domain rebuild must occur**: This can be triggered by:
   - CPU suspend/resume (calls `partition_sched_domains(1, NULL, NULL)` via `cpuset_cpu_inactive()`)
   - Cpuset configuration changes (modifying `cpuset.cpus` triggers `rebuild_sched_domains_locked()`)
   - CPU hotplug events (online/offline)
   - Any operation that calls `partition_sched_domains()` or `partition_sched_domains_locked()`

3. **A subsequent DEADLINE bandwidth check must occur**: After the domain rebuild inflates `total_bw`, a bandwidth overflow check must happen. This occurs during:
   - `dl_bw_deactivate()` when a CPU goes offline
   - `dl_bw_alloc()` when a new DEADLINE task is being admitted

4. **Topology must have sufficient special task bandwidth to cause overflow**: With sugov fake bandwidth of ~10% per kthread, a system with 4+ sugov kthreads in the same root domain would have 40%+ fake bandwidth. When CPUs start going offline (reducing capacity), the inflated `total_bw` can easily exceed the remaining capacity, triggering the overflow.

The bug is **deterministic** — it does not involve any race conditions or timing-sensitive behavior. Once the domain rebuild has inflated `total_bw` and a bandwidth check is performed, the failure is guaranteed if the inflated bandwidth exceeds the available capacity. On Jon Hunter's Tegra186 board with `isolcpus`, the failure was 100% reproducible on every suspend attempt.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP by creating a SCHED_DEADLINE "special" task (mimicking a sugov kthread), triggering a scheduling domain rebuild via cpuset changes, and then observing the incorrectly inflated `dl_bw.total_bw` on the root domain.

### Step-by-step plan:

1. **Topology Setup**: Configure QEMU with at least 2 CPUs (e.g., `--num_cpus 4`). Use `kstep_topo_init()` and `kstep_topo_apply()` to set up a basic topology. No special NUMA or SMT configuration is needed.

2. **Create a special DEADLINE task**: Create a kthread using `kstep_kthread_create("sugov_fake")`. Then use `sched_setattr_nocheck()` (already available as a kernel API, used in existing kSTEP drivers like `uclamp_inversion.c`) to set it as a SCHED_DEADLINE task with the `SCHED_FLAG_SUGOV` flag and fake bandwidth parameters:
   ```c
   struct sched_attr attr = {
       .size = sizeof(struct sched_attr),
       .sched_policy = SCHED_DEADLINE,
       .sched_flags = SCHED_FLAG_SUGOV,
       .sched_runtime = NSEC_PER_MSEC,
       .sched_deadline = 10 * NSEC_PER_MSEC,
       .sched_period = 10 * NSEC_PER_MSEC,
   };
   sched_setattr_nocheck(p, &attr);
   ```
   Bind it to CPU 1 using `kstep_kthread_bind()`. Start the kthread with `kstep_kthread_start()`.

3. **Record baseline bandwidth**: Before any domain rebuild, read the root domain's `dl_bw.total_bw` from `cpu_rq(1)->rd->dl_bw.total_bw` using kSTEP's internal access (via `internal.h` / `cpu_rq()`). Record this as `bw_before`. This should include only dl_server bandwidth (from fair_server), not the sugov fake bandwidth.

4. **Trigger domain rebuild via cpuset change**: Create a cpuset cgroup using `kstep_cgroup_create("test_cs")`. Then change its CPU mask using `kstep_cgroup_set_cpuset("test_cs", "1-3")` (or similar). This triggers `rebuild_sched_domains_locked()` → `partition_sched_domains_locked()` → `dl_rebuild_rd_accounting()`. The rebuild will iterate all tasks, including the fake sugov kthread, and call `dl_add_task_root_domain()` on it.

5. **Read bandwidth after rebuild**: Read `cpu_rq(1)->rd->dl_bw.total_bw` again as `bw_after`.

6. **Calculate expected fake bandwidth**: The fake sugov task's `dl_bw` is `runtime / period * (1 << DL_SCALE)` = `NSEC_PER_MSEC / (10 * NSEC_PER_MSEC) * (1 << 20)` = approximately `104857` (in DL_SCALE units). The `__dl_add` function adds `dl_bw / cpus_in_rd` to `total_bw`. So the fake contribution is `104857 / nr_cpus_in_rd`.

7. **Bug detection (pass/fail criteria)**:
   - **Buggy kernel**: `bw_after > bw_before` — the fake sugov bandwidth was incorrectly added to `total_bw`. Specifically, `bw_after - bw_before` should be approximately equal to the sugov task's `dl_bw / nr_cpus`. Report with `kstep_fail("total_bw inflated: before=%llu after=%llu delta=%llu", bw_before, bw_after, bw_after - bw_before)`.
   - **Fixed kernel**: `bw_after == bw_before` (within tolerance for dl_server re-accounting) — the special task was correctly skipped. Report with `kstep_pass("total_bw not inflated: before=%llu after=%llu", bw_before, bw_after)`.

8. **Optional: verify with multiple special tasks**: Create 2-3 fake sugov kthreads on different CPUs to amplify the effect. The inflated bandwidth should scale linearly with the number of special tasks.

9. **Callbacks**: No special tick or scheduler callbacks are needed. The domain rebuild is triggered synchronously by the cpuset change.

10. **Version guard**: Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,14,0)` since the buggy commit `53916d5fd3c0` was merged in v6.14-rc1 and the `dl_rebuild_rd_accounting` function (via cpuset.c) only exists in that version range. Also guard with `#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL` since `dl_entity_is_special()` returns `false` without it (meaning the bug and fix have no effect without schedutil).

### Key observations on buggy vs fixed kernel:
- **Buggy kernel (v6.14-rc1 through v6.14-rc5)**: After cpuset-triggered domain rebuild, `total_bw` includes the fake sugov bandwidth (~10% per kthread scaled by DL_SCALE). Multiple sugov kthreads make this more dramatic.
- **Fixed kernel (v6.15-rc1+)**: After the same domain rebuild, `total_bw` correctly excludes special task bandwidth. The `dl_entity_is_special()` check in `dl_add_task_root_domain()` causes the function to return early for the fake sugov kthread.

### Alternative approach (simpler):
Instead of observing `total_bw` directly, create a legitimate DEADLINE task alongside the fake sugov task, trigger a domain rebuild, then attempt to modify the DEADLINE task's parameters via `sched_setattr_nocheck()` with bandwidth close to capacity. On the buggy kernel, the admission would fail (because `total_bw` is inflated). On the fixed kernel, the admission would succeed. This approach tests the user-visible consequence rather than internal state.
