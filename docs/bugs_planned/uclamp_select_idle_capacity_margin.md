# Uclamp: select_idle_capacity() Ignores Migration Margin / Capacity Pressure Interaction

**Commit:** `b759caa1d9f667b94727b2ad12589cbc4ce13a82`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.2-rc1
**Buggy since:** v5.10-rc4 (introduced by `b4c9c9f15649` "sched/fair: Prefer prev cpu in asymmetric wakeup path")

## Bug Description

On asymmetric CPU capacity systems (e.g., ARM big.LITTLE), `select_idle_capacity()` is called from `select_idle_sibling()` during task wakeup to find an idle CPU that can fit the task. The function must determine whether a given CPU has enough capacity to run the waking task. When utilization clamping (uclamp) is active, the function must consider both the task's actual utilization and its uclamp bounds (minimum and maximum utilization constraints) when deciding if a CPU "fits."

The bug is that `select_idle_capacity()` used the old `uclamp_task_util()` function followed by `fits_capacity()` to make this determination, rather than the new `util_fits_cpu()` function that correctly handles the interaction between uclamp bounds and the migration margin. The `fits_capacity()` macro adds a ~20% migration margin (`cap * 1280 < max * 1024`) to account for utilization growth during migration. When this margin is applied to a uclamp-clamped utilization value, it produces incorrect results: a task boosted via `uclamp_min` to a capacity level equal to a CPU's original capacity will never "fit" that CPU because the 20% margin inflates the comparison beyond the CPU's capacity.

This leads to incorrect CPU selection during wakeup on asymmetric capacity systems with uclamp enabled. Tasks may be unnecessarily migrated to larger CPUs when they should fit on smaller or medium-capacity CPUs, defeating the purpose of uclamp placement hints and increasing energy consumption.

The fix is part of a 9-patch series by Qais Yousef titled "Fix relationship between uclamp and fits_capacity()" that systematically replaces all `fits_capacity()` call sites with the new `util_fits_cpu()` function when uclamp is in use.

## Root Cause

The root cause lies in how `select_idle_capacity()` conflates the task's actual utilization signal with its uclamp bounds into a single number, then applies the migration margin indiscriminately to that combined value.

**Before the fix**, `select_idle_capacity()` calls:
```c
task_util = uclamp_task_util(p);
```
where `uclamp_task_util(p)` is defined as:
```c
static inline unsigned long uclamp_task_util(struct task_struct *p)
{
    return clamp(task_util_est(p),
                 uclamp_eff_value(p, UCLAMP_MIN),
                 uclamp_eff_value(p, UCLAMP_MAX));
}
```
This clamps the task's estimated utilization between its effective uclamp_min and uclamp_max, producing a single value. Then the function checks:
```c
if (fits_capacity(task_util, cpu_cap))
    return cpu;
```
where `fits_capacity()` is:
```c
#define fits_capacity(cap, max) ((cap) * 1280 < (max) * 1024)
```

The problem manifests in several scenarios:

1. **Boosted task (uclamp_min = capacity_orig_of(cpu))**: If a task has `util_avg = 200` and `uclamp_min = 768` (matching a medium CPU's capacity), then `uclamp_task_util()` returns 768. Now `fits_capacity(768, 768)` evaluates to `768 * 1280 < 768 * 1024` → `983040 < 786432` → **false**. The task does not "fit" the medium CPU despite the uclamp_min indicating it should be placed there. The task gets pushed to a bigger CPU unnecessarily.

2. **Max-boosted task (uclamp_min = 1024)**: If a task has `util_avg = 300` and `uclamp_min = 1024`, then `uclamp_task_util()` returns 1024. `fits_capacity(1024, 1024)` = `1024 * 1280 < 1024 * 1024` → `1310720 < 1048576` → **false**. The task cannot fit on ANY CPU because the migration margin makes the max capacity insufficient for the max-clamped utilization.

3. **Capacity pressure**: `capacity_of(cpu)` already accounts for capacity pressure (IRQ time, thermal, etc.), which means even slight pressure on a big CPU makes it impossible for a max-boosted task to fit, as the margin is applied on top of a reduced capacity value.

**After the fix**, the function separates the concerns:
```c
task_util = task_util_est(p);
util_min = uclamp_eff_value(p, UCLAMP_MIN);
util_max = uclamp_eff_value(p, UCLAMP_MAX);
```
and calls:
```c
if (util_fits_cpu(task_util, util_min, util_max, cpu))
    return cpu;
```

The `util_fits_cpu()` function handles each component correctly:
- The **actual utilization** (`task_util_est`) is checked against `capacity_of(cpu)` (which includes capacity pressure) using `fits_capacity()` with the migration margin.
- The **uclamp_min** bound is checked against `capacity_orig_of(cpu)` (minus thermal pressure only) **without** the migration margin.
- The **uclamp_max** bound is checked against `capacity_orig_of(cpu)` **without** the migration margin or capacity pressure.

This separation ensures that uclamp placement hints are respected without the migration margin interfering.

## Consequence

The observable impact is **incorrect task placement on asymmetric CPU capacity systems when uclamp is active**. Specifically:

- **Tasks boosted via uclamp_min are placed on unnecessarily large CPUs.** A task that should fit on a medium-capacity CPU gets migrated to a big CPU because the 20% migration margin makes the clamped utilization value exceed the medium CPU's capacity. This wastes energy on battery-powered devices (the primary use case for big.LITTLE systems) by running tasks on power-hungry big cores when the medium cores would suffice.

- **Tasks capped via uclamp_max may not be placed on the intended CPU.** If a task is capped to the capacity of a small CPU, the combined uclamp_task_util + fits_capacity check may incorrectly reject that CPU. The task falls through to the `best_cpu` fallback path, potentially landing on a larger CPU than intended, which contradicts the energy-saving intent of uclamp_max.

- **The `best_cpu` fallback path is triggered unnecessarily.** When no CPU appears to "fit" (because the migration margin rejects all candidates), `select_idle_capacity()` returns `best_cpu` — the idle CPU with the highest capacity. This is a reasonable fallback, but it means the task always migrates to the biggest available CPU rather than staying on a smaller one that is actually sufficient. On systems with many CPU capacity levels, this concentrates work on big cores, increasing power draw and potentially causing thermal throttling.

The severity is moderate: there is no crash, hang, or data corruption. The impact is suboptimal scheduling decisions that increase energy consumption and may degrade performance through thermal throttling on mobile and embedded platforms using ARM big.LITTLE or DynamIQ CPU configurations with uclamp-aware workloads (e.g., Android's ADPF framework).

## Fix Summary

The fix changes `select_idle_capacity()` to use the newly introduced `util_fits_cpu()` function instead of the combination of `uclamp_task_util()` + `fits_capacity()`.

Specifically, the fix makes three changes:
1. Replaces `task_util = uclamp_task_util(p)` with three separate values:
   - `task_util = task_util_est(p)` — the raw task utilization estimate without uclamp clamping
   - `util_min = uclamp_eff_value(p, UCLAMP_MIN)` — the effective uclamp minimum
   - `util_max = uclamp_eff_value(p, UCLAMP_MAX)` — the effective uclamp maximum
2. Replaces `fits_capacity(task_util, cpu_cap)` with `util_fits_cpu(task_util, util_min, util_max, cpu)`.

The `util_fits_cpu()` function (introduced in a preceding patch in the same series) properly handles the interaction:
- For the **real utilization** (`task_util_est`), it uses `fits_capacity()` against `capacity_of(cpu)`, preserving the migration margin and capacity pressure effects for the actual workload signal.
- For **uclamp_max**, it checks whether `uclamp_max <= capacity_orig_of(cpu)` (without migration margin), forcing the task to fit on CPUs where the cap is at or below their original capacity. An exception is made when the CPU is at max capacity (`SCHED_CAPACITY_SCALE`) and uclamp_max is also at max, to avoid blocking the overutilized state.
- For **uclamp_min** (boost), it checks whether `uclamp_min <= capacity_orig_thermal` (original capacity minus thermal pressure, without migration margin), ensuring boosted tasks are placed on CPUs that can actually deliver the requested minimum performance level.

This fix is correct and complete for the `select_idle_capacity()` call site. When uclamp is not in use (`!uclamp_is_used()`), `util_fits_cpu()` falls back to a simple `fits_capacity()` call, so there is no behavior change for systems without uclamp.

## Triggering Conditions

To trigger this bug, the following conditions must all be met:

1. **Asymmetric CPU capacity system**: The system must have CPUs with different original capacities (e.g., big.LITTLE), and `sched_asym_cpucap_active()` must return true. This requires the `SD_ASYM_CPUCAPACITY` flag to be set in the scheduling domain hierarchy, which happens automatically when CPUs report different capacities via `arch_scale_cpu_capacity()`.

2. **CONFIG_UCLAMP_TASK=y**: The kernel must be compiled with utilization clamping support enabled. `uclamp_is_used()` must also return true, meaning at least one task has non-default uclamp values.

3. **Task with uclamp_min >= capacity_orig of a smaller CPU**: A CFS task must have its `uclamp_min` set to a value equal to or greater than the original capacity of a medium or small CPU. For example, on a system with little CPUs (capacity 512) and big CPUs (capacity 1024), a task with `uclamp_min = 512` or higher will exhibit the bug. The most dramatic case is `uclamp_min = 1024`, where the task cannot fit on ANY CPU.

4. **Task's actual utilization is low**: The task's `task_util_est()` must be significantly lower than its uclamp_min, so the clamping actually increases the comparison value. If the task's actual utilization is already high enough to not fit, the bug is masked by the correct behavior of the raw utilization check.

5. **The task must be waking up**: The bug is in the wakeup path — specifically in `select_idle_sibling()` → `select_idle_capacity()`. The task must be blocked and then woken up to trigger this code path.

6. **An idle CPU of the correct capacity must exist**: There must be an idle CPU whose original capacity is >= the task's uclamp_min but whose capacity fails the `fits_capacity()` check due to the migration margin (i.e., `capacity_orig * 1024 <= uclamp_min * 1280`). For example, a medium CPU with capacity 820 and a task with uclamp_min 820: `fits_capacity(820, 820)` = `820*1280 < 820*1024` = false.

7. **A bigger idle CPU must exist**: For the incorrect behavior to be observable, there must also be an idle big CPU with enough capacity to pass the `fits_capacity()` check, so the task migrates there instead. If no bigger CPU exists, the task falls through to `best_cpu` which still selects the same CPU, making the bug effect invisible.

The bug is deterministic given these conditions — no race condition or specific timing is required. Every wakeup of a uclamp-boosted task on an asymmetric system will trigger the incorrect code path.

## Reproduce Strategy (kSTEP)

This bug is reproducible with kSTEP. The strategy uses asymmetric CPU capacities, uclamp configuration, and task wakeup observation to demonstrate incorrect CPU placement on the buggy kernel versus correct placement on the fixed kernel.

### Step 1: QEMU Configuration

Configure QEMU with at least 3 CPUs (CPU 0 is reserved for the driver, so we need CPUs 1 and 2 as the target CPUs with different capacities). Ideally use 4 CPUs: CPU 0 (driver), CPU 1 (little, capacity 512), CPU 2 (medium, capacity 768), CPU 3 (big, capacity 1024).

### Step 2: Topology Setup

Use `kstep_topo_init()` and `kstep_topo_*` functions to set up the asymmetric topology. The key requirement is that CPUs with different capacities are in the same `sd_asym_cpucapacity` domain:

```c
kstep_topo_init();
// All CPUs in one LLC domain with asymmetric capacities
const char *mc[] = {"0-3", NULL};
kstep_topo_set_mc(mc, 1);
kstep_topo_apply();

// Set asymmetric capacities
kstep_cpu_set_capacity(1, 512);   // little CPU
kstep_cpu_set_capacity(2, 768);   // medium CPU
kstep_cpu_set_capacity(3, 1024);  // big CPU
```

This should cause `sched_asym_cpucap_active()` to return true and ensure `sd_asym_cpucapacity` is set for the wakeup path.

### Step 3: Task Creation and Uclamp Configuration

Create a CFS task and set its uclamp_min to match the medium CPU's capacity (768), so the task should fit on CPU 2 but the buggy code will push it to CPU 3:

```c
struct task_struct *task = kstep_task_create();
kstep_task_pin(task, 1, 3);  // allow CPUs 1-3

// Set uclamp using sched_setattr_nocheck (as in uclamp_inversion.c)
struct sched_attr attr = {
    .size = sizeof(attr),
    .sched_policy = SCHED_NORMAL,
    .sched_flags = SCHED_FLAG_UTIL_CLAMP,
    .sched_util_min = 768,  // boost to medium capacity
    .sched_util_max = 1024,
};
sched_setattr_nocheck(task, &attr);
```

Also need to set up a cgroup with uclamp enabled for the task:
```c
kstep_cgroup_create("test");
kstep_cgroup_write("test", "cpu.uclamp.min", "75.00");  // ~768/1024
kstep_cgroup_write("test", "cpu.uclamp.max", "100.00");
kstep_cgroup_add_task("test", task->pid);
```

### Step 4: Build Up Low Task Utilization

The task needs a low `task_util_est()` (e.g., 200) so that the raw utilization fits on the medium CPU but the uclamp-clamped value does not (due to migration margin). Run the task briefly then block it:

```c
kstep_task_wakeup(task);
kstep_tick_repeat(5);  // let it run briefly to establish low util
kstep_task_block(task);
kstep_tick_repeat(10); // let util decay slightly
```

### Step 5: Ensure CPUs 2 and 3 Are Idle

Before the critical wakeup, ensure CPUs 2 (medium) and 3 (big) are idle. Since we only have one task and it's blocked, they should be idle.

### Step 6: Trigger Wakeup and Observe Placement

Wake the task and immediately check which CPU it was placed on:

```c
kstep_task_wakeup(task);
int placed_cpu = task_cpu(task);
unsigned long util = task_util_est(task);
unsigned long umin = uclamp_eff_value(task, UCLAMP_MIN);
unsigned long cap_medium = capacity_orig_of(2);  // 768
```

### Step 7: Determine Pass/Fail

On the **buggy kernel**:
- `uclamp_task_util(task)` returns `clamp(util, 768, 1024)` = 768 (assuming util < 768)
- `fits_capacity(768, 768)` = `768*1280 < 768*1024` = `983040 < 786432` = **false**
- The medium CPU is rejected; task lands on big CPU 3
- `placed_cpu == 3` → **FAIL** (incorrect placement)

On the **fixed kernel**:
- `task_util_est(task)` returns the raw util (e.g., 200)
- `util_fits_cpu(200, 768, 1024, 2)`:
  - `fits_capacity(200, capacity_of(2))` = `200*1280 < 768*1024` = `256000 < 786432` = true
  - uclamp_min check: `200 < 768 && capacity_orig(768) != 1024` → `768 <= 768` = true
  - Returns true → task fits on medium CPU 2
- `placed_cpu == 2` → **PASS** (correct placement)

```c
if (placed_cpu == 2) {
    kstep_pass("Task correctly placed on medium CPU %d (util=%lu, uclamp_min=%lu)", placed_cpu, util, umin);
} else if (placed_cpu == 3) {
    kstep_fail("Task incorrectly placed on big CPU %d (util=%lu, uclamp_min=%lu, cap_medium=%lu)", placed_cpu, util, umin, cap_medium);
} else {
    kstep_fail("Task placed on unexpected CPU %d", placed_cpu);
}
```

### Step 8: Additional Logging

Add detailed logging in `on_tick_begin` or after wakeup to capture:
- `task_util_est(task)`, `uclamp_eff_value(task, UCLAMP_MIN)`, `uclamp_eff_value(task, UCLAMP_MAX)`
- `capacity_of(cpu)` and `capacity_orig_of(cpu)` for each CPU
- `task_cpu(task)` after each wakeup
- Whether `sched_asym_cpucap_active()` is true

### Step 9: Version Guard

Guard the driver with `#if LINUX_VERSION_CODE` to ensure it runs on kernels in the affected range (v5.15 through the fix):

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
// driver code
#endif
```

### Step 10: kSTEP Requirements

The driver requires:
- `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y` (verified present in kSTEP's kernel config)
- `sched_setattr_nocheck()` to set per-task uclamp values (available as a kernel symbol, already used in `uclamp_inversion.c` driver)
- At least 4 CPUs in QEMU (CPU 0 for driver, CPUs 1-3 for asymmetric topology)
- The `select_idle_capacity()` path is only entered when `sched_asym_cpucap_active()` is true and `sd_asym_cpucapacity` is set — this requires the topology setup to correctly create asymmetric sched domains

The main concern is ensuring `kstep_cpu_set_capacity()` triggers a sched domain rebuild with `SD_ASYM_CPUCAPACITY` set. If this does not happen automatically, the driver may need to call `kstep_topo_apply()` after setting capacities, or the kSTEP framework may need a minor extension to force the asymmetric cpucapacity key to be set. Verify this by checking `sched_asym_cpucap_active()` after topology setup.
