# Energy: find_energy_efficient_cpu() Skips EAS When uclamp_max Is 0

**Commit:** `23c9519def98ee0fa97ea5871535e9b136f522fc`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.7-rc1
**Buggy since:** v6.2-rc1 (commit `d81304bc6193` "sched/uclamp: Cater for uclamp in find_energy_efficient_cpu()'s early exit condition")

## Bug Description

The `find_energy_efficient_cpu()` (feec) function in the scheduler's Energy Aware Scheduling (EAS) path has an early exit optimization that skips energy computation when the task's effective utilization is zero, since such a task would contribute no energy delta and EAS has nothing to optimize. However, when `CONFIG_UCLAMP_TASK` is enabled and a task has its `uclamp_max` set to 0, this optimization incorrectly causes EAS to skip energy computation for tasks that are actually running and consuming energy.

The problem was introduced by commit `d81304bc6193` which attempted to make feec's early exit condition uclamp-aware. That commit changed the check from `if (!task_util_est(p))` to `if (!uclamp_task_util(p, p_util_min, p_util_max))`, where `uclamp_task_util()` clamps `task_util_est(p)` between `uclamp_min` and `uclamp_max`. When `uclamp_max == 0` and `task_util_est(p) > 0`, the clamped value is `clamp(nonzero, 0, 0) == 0`, causing feec to bail out. The intent of the original commit was to prevent skipping EAS for tasks boosted by `uclamp_min > 0`, but it inadvertently created the opposite problem: tasks capped by `uclamp_max == 0` are now always skipped.

When a task has `uclamp_max = 0`, it is capped to the lowest performance point, but it is still actively running and consuming energy. EAS should still compute energy for this task to determine the most energy-efficient CPU placement. By skipping the energy computation, the scheduler falls back to returning `prev_cpu` without considering whether a different CPU would be more energy-efficient, potentially wasting energy by keeping the task on a high-capacity (big) CPU when a low-capacity (LITTLE) CPU would be both sufficient and more efficient.

This bug was identified by Qais Yousef and reported as part of a 3-patch series (v5) that addressed two corner cases in feec when using `uclamp_max`. The series went through five iterations of review involving Vincent Guittot, Dietmar Eggemann, and Peter Zijlstra before being merged.

## Root Cause

The root cause is a logic error in the early-exit condition of `find_energy_efficient_cpu()` introduced by commit `d81304bc6193`. The relevant code path is in `kernel/sched/fair.c` within the `find_energy_efficient_cpu()` function.

Before the buggy commit, the early exit was:
```c
sync_entity_load_avg(&p->se);
if (!task_util_est(p))
    goto unlock;
```
This checked if the task's estimated utilization was zero. If so, the task contributes zero energy delta to any CPU, so EAS can be skipped. This was correct.

The buggy commit changed this to:
```c
sync_entity_load_avg(&p->se);
if (!uclamp_task_util(p, p_util_min, p_util_max))
    goto unlock;
```
Where `uclamp_task_util()` was defined as:
```c
static inline unsigned long uclamp_task_util(struct task_struct *p,
                                             unsigned long uclamp_min,
                                             unsigned long uclamp_max)
{
    return clamp(task_util_est(p), uclamp_min, uclamp_max);
}
```
And `p_util_min` and `p_util_max` are computed at the top of feec:
```c
unsigned long p_util_min = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MIN) : 0;
unsigned long p_util_max = uclamp_is_used() ? uclamp_eff_value(p, UCLAMP_MAX) : 1024;
```

The logic error is as follows: when `uclamp_max = 0`, the `clamp()` call returns 0 regardless of the actual task utilization. If `task_util_est(p) = 500` and `uclamp_max = 0`, then `clamp(500, 0, 0) = 0`, so `!uclamp_task_util(...)` evaluates to true, and feec returns early.

The original intent of using `uclamp_task_util()` was to prevent the early exit when `uclamp_min > 0` (boosted tasks). A task with `task_util_est(p) = 0` but `uclamp_min = 200` should NOT have its EAS computation skipped, because the boost means it will affect frequency selection and energy. The buggy code correctly handles this case: `clamp(0, 200, 1024) = 200`, which is nonzero, so EAS proceeds. But it fails for the complementary case where `uclamp_max = 0`.

The function `uclamp_task_util()` conflates two different concepts: (1) the task's actual energy footprint (which depends on its real utilization and busy time), and (2) the task's clamped utilization (which affects frequency selection). The early-exit optimization needs to consider the former — whether the task actually contributes energy — not the latter.

## Consequence

The observable impact is suboptimal CPU placement for tasks with `uclamp_max = 0` on systems with Energy Aware Scheduling enabled (big.LITTLE or DynamIQ ARM platforms).

When the bug triggers, feec returns `prev_cpu` without performing any energy computation. This means the task stays on whichever CPU it previously ran on, regardless of whether that CPU is the most energy-efficient choice. In the worst case, a task that is capped to the lowest performance point (uclamp_max = 0) remains running on a big (high-capacity) CPU when a LITTLE (low-capacity, more power-efficient) CPU would suffice. This wastes energy because the big CPU consumes more power at any given utilization level than the LITTLE CPU.

On Android and ChromeOS devices (the primary EAS platforms), `uclamp_max` is actively used by power management frameworks like Android's `SchedTune` or `thermal-engine` to throttle background tasks and limit their performance impact. Setting `uclamp_max = 0` is used to indicate "this task should run at the lowest possible performance point." When the bug prevents EAS from placing such tasks optimally, the device consumes more power, reducing battery life. While the magnitude depends on workload composition and how aggressively `uclamp_max = 0` is used, the per-task energy waste compounds over many such tasks and over time.

There is no crash, panic, or functional incorrectness beyond suboptimal placement. The task still runs correctly; it simply runs on a potentially less efficient CPU. This is strictly an energy efficiency regression, not a correctness or stability issue.

## Fix Summary

The fix replaces the single `uclamp_task_util()` check with two independent conditions and removes the now-unused `uclamp_task_util()` function entirely. The new code is:

```c
sync_entity_load_avg(&p->se);
if (!task_util_est(p) && p_util_min == 0)
    goto unlock;
```

This change decouples the two cases that should allow feec to skip energy computation:

1. **`!task_util_est(p)`**: The task's actual estimated utilization is zero. A task with zero utilization generates zero busy time and contributes no energy delta to any CPU.
2. **`p_util_min == 0`**: The task is not boosted. If `uclamp_min > 0`, the task's boosted utilization will affect frequency selection and thus energy, even if the task's actual utilization is zero.

Both conditions must be true for feec to bail out. If the task has nonzero utilization (`task_util_est(p) > 0`), feec always proceeds regardless of `uclamp_max`. If the task has zero utilization but is boosted (`p_util_min > 0`), feec also proceeds because the boost affects energy.

The case that previously failed — `task_util_est(p) > 0, uclamp_max = 0` — now correctly proceeds: `!task_util_est(p)` is false (since util is nonzero), so the short-circuit AND prevents the early exit. EAS runs and can compute the optimal placement for this capped-but-running task.

The fix also removes the `uclamp_task_util()` function entirely (both the `CONFIG_UCLAMP_TASK` and non-uclamp versions), since it is no longer used anywhere. This cleanup eliminates dead code and prevents future misuse of the function. The fix is minimal, correct, and was reviewed and acknowledged by all major scheduler maintainers (Vincent Guittot, Dietmar Eggemann, Peter Zijlstra).

## Triggering Conditions

The following conditions must ALL be met to trigger the bug:

- **Energy Aware Scheduling must be enabled**: EAS requires `CONFIG_ENERGY_MODEL=y`, `CONFIG_CPU_FREQ=y`, an asymmetric CPU capacity topology (different CPU capacities exist), and a registered Energy Model (EM) with performance domains. The `sched_energy_present` static key must be enabled. This rules out all symmetric multiprocessor systems (most x86 servers/desktops). EAS is primarily active on ARM big.LITTLE and DynamIQ platforms (Android, ChromeOS).

- **Utilization clamping must be enabled**: `CONFIG_UCLAMP_TASK=y` must be set, and uclamp must be in active use. The `uclamp_is_used()` check at the top of feec must return true, which requires that at least one task or cgroup has set a uclamp value.

- **A task must have `uclamp_max = 0`**: The effective uclamp_max value (`uclamp_eff_value(p, UCLAMP_MAX)`) must be 0. This can be set per-task via `sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP_MAX` or via cgroup `cpu.uclamp.max = 0.00`. The effective value considers both the task's requested value and the cgroup hierarchy limits.

- **The task must have non-zero utilization**: `task_util_est(p)` must be greater than 0. This means the task must have run recently (building up PELT utilization or util_est) before being woken with `uclamp_max = 0`. A newly created task that has never run would have zero utilization and would correctly be skipped by both buggy and fixed code.

- **The system must not be overutilized**: `rd->overutilized` must be false. If the system is overutilized, feec bails out even before reaching the buggy check. This requires that total CPU utilization across all CPUs is within capacity.

- **The task must be woken up (not a new task fork)**: The `find_energy_efficient_cpu()` is called from `select_task_rq_fair()` during task wakeup (`SD_BALANCE_WAKE`), not during fork.

- **No special timing or race condition**: The bug is deterministic. Every wakeup of a task meeting the above conditions triggers the early exit in feec. There is no race condition or timing sensitivity.

- **Number of CPUs**: At least 2 CPUs with different capacities are needed for asymmetric scheduling. A typical EAS platform has 4-8 CPUs across 2-3 clusters (e.g., 4 LITTLE + 4 big).

## Reproduce Strategy (kSTEP)

Reproducing this bug in kSTEP requires setting up a functional Energy Aware Scheduling environment, which requires extensions to the kSTEP framework. Below is a detailed plan.

### Required kSTEP Extensions

1. **Energy Model Registration**: Add a helper `kstep_em_register(cpumask, nr_states, power_table)` that calls `em_dev_register_perf_domain()` for a set of CPUs with fake performance states. The helper should:
   - Accept a CPU mask and a table of `(frequency, power)` pairs
   - Obtain CPU device pointers via `get_cpu_device(cpu)` for the first CPU in the mask
   - Create a `struct em_data_callback` with an `active_power` callback that returns power from the table
   - Call `em_dev_register_perf_domain(dev, nr_states, cb, cpumask, true)` (milliwatts=true)
   - This function is `EXPORT_SYMBOL_GPL` and callable from modules; alternatively, use `KSYM_IMPORT(em_dev_register_perf_domain)`

2. **Kernel Configuration**: The kSTEP kernel config must include `CONFIG_ENERGY_MODEL=y`, `CONFIG_UCLAMP_TASK=y`, and `CONFIG_CPU_FREQ=y`. If these are not currently enabled, the kernel config needs updating.

### Topology and EM Setup (setup function)

1. **Configure 4 CPUs with asymmetric capacity**: Use `kstep_cpu_set_capacity()` to create two clusters:
   - CPUs 1-2: LITTLE cluster, capacity = 512 (SCHED_CAPACITY_SCALE / 2)
   - CPUs 3-4: big cluster, capacity = 1024 (SCHED_CAPACITY_SCALE)
   - CPU 0 is reserved for the driver

2. **Set up topology**: Use `kstep_topo_init()`, `kstep_topo_set_cls()` with two clusters `{"1-2", "3-4"}`, then `kstep_topo_apply()`.

3. **Register Energy Models**: Before calling `kstep_topo_apply()`, register fake EMs:
   - LITTLE cluster (CPUs 1-2): e.g., 2 OPPs: (500MHz, 100mW), (1000MHz, 300mW)
   - big cluster (CPUs 3-4): e.g., 2 OPPs: (500MHz, 200mW), (1000MHz, 800mW)
   - The key is that the big cluster uses more power per OPP than the LITTLE cluster

4. **Rebuild sched domains**: Call `kstep_topo_apply()` which triggers `partition_sched_domains_locked()` → `build_perf_domains()`. This should detect the registered EMs and create `struct perf_domain` entries linked from `rd->pd`.

5. **Verify EAS is enabled**: After setup, check that `this_rq()->rd->pd != NULL` and optionally that `sched_energy_enabled()` returns true (via `KSYM_IMPORT(sched_energy_present)` static key check). If not, the EM registration or topology setup failed.

### Task Setup

6. **Create the test task**: Use `kstep_task_create()` to create a CFS task.

7. **Build utilization**: Wake the task on a big CPU (e.g., CPU 3) and let it run for many ticks (`kstep_tick_repeat(200+)`) to build up PELT utilization (`task_util_est(p) > 0`). Pin it to CPU 3 initially using `kstep_task_pin(task, 3, 4)` so prev_cpu = 3 (a big CPU).

8. **Set uclamp_max = 0**: Use `sched_setattr_nocheck()` as in the existing `uclamp_inversion.c` driver:
   ```c
   struct sched_attr attr = {
       .size = sizeof(attr),
       .sched_policy = SCHED_NORMAL,
       .sched_flags = SCHED_FLAG_UTIL_CLAMP_MAX,
       .sched_util_max = 0,
   };
   sched_setattr_nocheck(task, &attr);
   ```

### Triggering the Bug

9. **Block and re-wake the task**: Use `kstep_task_block(task)` to dequeue the task, then remove the CPU pin to allow EAS to choose freely. Then use `kstep_task_wakeup(task)` to trigger the wakeup path which calls `select_task_rq_fair()` → `find_energy_efficient_cpu()`.

10. **Observe CPU placement**: After wakeup, check `task_cpu(task)`:
    - **Buggy kernel**: feec returns `prev_cpu` (CPU 3, big) because `uclamp_task_util()` returns 0 and the early exit triggers. The task stays on the big CPU.
    - **Fixed kernel**: feec proceeds, computes energy for all CPUs, and should select a LITTLE CPU (CPU 1 or 2) since the task's effective utilization is 0 (capped by uclamp_max=0) and running it on a LITTLE CPU saves energy.

### Detection Logic

11. **Pass/fail criteria**: Read `task_cpu(task)` after the wakeup:
    - If the task is placed on a LITTLE CPU (1 or 2): **PASS** (EAS ran and chose energy-efficient placement)
    - If the task remains on big CPU (3 or 4): **FAIL** (EAS was skipped due to the bug)

    Additionally, for more robust detection:
    - Read `task->se.avg.util_avg` before the wakeup to confirm it's nonzero
    - Read `uclamp_eff_value(task, UCLAMP_MAX)` to confirm it's 0
    - Check `this_rq()->rd->pd != NULL` to confirm EAS is active
    - Check `!READ_ONCE(this_rq()->rd->overutilized)` to confirm system is not overutilized

12. **Alternative detection**: If CPU placement is unreliable (other scheduling decisions may override EAS), an alternative approach is to directly instrument the code path:
    - Use `KSYM_IMPORT` to access the `find_energy_efficient_cpu` function's behavior indirectly
    - Or add a tracepoint/printk in the kernel near the early exit to check whether it fires
    - Or read `rd->pd` and manually walk the perf domains to verify compute_energy would have been called

### Caveats and Notes

- **QEMU CPU count**: Configure QEMU with at least 5 CPUs (0 reserved for driver, 1-2 LITTLE, 3-4 big).
- **CONFIG_ENERGY_MODEL**: Must be enabled in kSTEP's kernel build configuration. If currently disabled, this config change is a prerequisite.
- **EM registration timing**: The EM must be registered BEFORE the sched domain rebuild that enables EAS. If kSTEP calls `kstep_topo_apply()` before EM registration, perf domains won't be created.
- **Task utilization buildup**: The task must have run enough to have nonzero `task_util_est()`. A fresh task with zero utilization would correctly be skipped by both buggy and fixed kernels. Running for ~200 ticks should be sufficient.
- **Kernel version guard**: Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 7, 0)` to match the buggy window.
- **Overutilization**: Ensure the system is not overutilized by keeping total utilization low. Only the test task should be running on the non-driver CPUs, and its utilization should be well below total capacity.
