# Uclamp: iowait boost escapes uclamp_max restriction

**Commit:** `d37aee9018e68b0d356195caefbb651910e0bbfa`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v5.18-rc1
**Buggy since:** v5.3-rc1 (introduced by commit 982d9cdc22c9 "sched/cpufreq, sched/uclamp: Add clamps for FAIR and RT tasks")

## Bug Description

The schedutil cpufreq governor maintains an `iowait_boost` mechanism that temporarily raises CPU frequency when a task wakes from I/O wait. This boost is designed to speed up I/O-bound workloads by proactively increasing frequency so that the next burst of computation after I/O completes runs at a higher speed. The boost is applied inside `sugov_iowait_apply()`, which sets `sg_cpu->util` to the boost value if it exceeds the current utilization.

The utilization clamping (uclamp) subsystem allows userspace to restrict the maximum utilization signal that the scheduler and cpufreq governor use for frequency selection. A task's `uclamp_max` cap is designed to ensure that the task (and its runqueue) never drives the CPU frequency above a specified threshold. This is critical for power management on mobile and embedded systems, where background or low-priority I/O-heavy tasks should not drive the CPU to maximum frequency.

The bug is that `sugov_iowait_apply()` computes and applies the iowait boost value without passing it through `uclamp_rq_util_with()`. While `effective_cpu_util()` — the primary utilization aggregation function — correctly clamps its result via `uclamp_rq_util_with()`, the iowait boost path is completely independent and bypasses this clamping. As a result, an I/O-heavy task capped by `uclamp_max` to a low frequency can still cause the CPU to run at maximum frequency, because the raw iowait_boost value is written directly into `sg_cpu->util` without any uclamp filtering.

This is part of a two-patch series by Qais Yousef. Patch 1/2 (commit 7a17e1db1265) fixes a separate but related issue where the schedutil "busy" filter prevents uclamp_max from taking effect. Patch 2/2 (this commit) fixes the iowait_boost bypass specifically. Both patches address the same root problem: uclamp_max restrictions being ignored in schedutil code paths.

## Root Cause

The root cause is a missing call to `uclamp_rq_util_with()` in the `sugov_iowait_apply()` function in `kernel/sched/cpufreq_schedutil.c`.

When schedutil performs a frequency update, it computes the CPU's effective utilization via `effective_cpu_util()`, which aggregates CFS, RT, DL, and IRQ utilization and then passes the result through `uclamp_rq_util_with()` to respect uclamp constraints. However, `sugov_iowait_apply()` is called separately after the utilization computation. It computes the boost value as:

```c
boost = (sg_cpu->iowait_boost * sg_cpu->max) >> SCHED_CAPACITY_SHIFT;
```

This converts `iowait_boost` (which ranges from `IOWAIT_BOOST_MIN` up to the CPU's maximum capacity) into the same capacity scale as `sg_cpu->util`. The function then directly compares and potentially overwrites `sg_cpu->util`:

```c
if (sg_cpu->util < boost)
    sg_cpu->util = boost;
```

Because the `boost` value is never passed through `uclamp_rq_util_with()`, it completely ignores any `uclamp_max` restriction on the runqueue. Even if all tasks on the CPU have `uclamp_max` set to a very low value (e.g., 0), the iowait_boost can push `sg_cpu->util` up to `sg_cpu->max` (i.e., full capacity), which in turn causes schedutil to request the maximum CPU frequency.

The `iowait_boost` value is set in `sugov_iowait_boost()` which is called when the `SCHED_CPUFREQ_IOWAIT` flag is present during a scheduler utilization update. This flag is set by `enqueue_task_fair()` when a task wakes from I/O wait (i.e., `p->in_iowait` is set). The boost doubles on each consecutive IO wakeup up to the CPU's maximum capacity, meaning a sustained I/O workload can keep the boost at maximum indefinitely.

The original commit 982d9cdc22c9 ("sched/cpufreq, sched/uclamp: Add clamps for FAIR and RT tasks") added uclamp support to `effective_cpu_util()` but failed to account for the separate iowait_boost code path in `sugov_iowait_apply()`. This was an oversight — the iowait boost path was not integrated with the uclamp framework when uclamp was initially added.

## Consequence

The primary consequence is that `uclamp_max` becomes ineffective for I/O-heavy tasks. A task that performs frequent I/O operations will trigger repeated iowait_boost increases, and each wake-from-IO will double the boost until it reaches the CPU's maximum capacity. Despite the task having a `uclamp_max` of, say, 0 or 400 (out of 1024), the CPU frequency can be driven to its absolute maximum.

This defeats the purpose of uclamp_max for power management. On mobile devices and embedded systems, uclamp_max is used to cap background tasks to low frequencies to save battery power and reduce heat generation. An I/O-heavy background task (e.g., a download service, log rotation, database sync) that is supposed to be capped will instead drive the CPU to max frequency every time it wakes from I/O. On a laptop, the author demonstrated that with `uclampset -M 0 sysbench --test=cpu --threads=4 run`, the system still runs at the highest frequency (~3.1GHz on a 2-core SMT2 Intel laptop) instead of being throttled down.

There is no crash, hang, or data corruption. The consequence is purely a performance/power regression: the system uses more power, generates more heat, and ignores administrator or framework frequency capping directives. On battery-powered devices, this can lead to measurably reduced battery life and thermal throttling.

## Fix Summary

The fix adds a single line to `sugov_iowait_apply()` that passes the computed `boost` value through `uclamp_rq_util_with()` before comparing it with `sg_cpu->util`:

```c
boost = (sg_cpu->iowait_boost * sg_cpu->max) >> SCHED_CAPACITY_SHIFT;
boost = uclamp_rq_util_with(cpu_rq(sg_cpu->cpu), boost, NULL);
if (sg_cpu->util < boost)
    sg_cpu->util = boost;
```

The call `uclamp_rq_util_with(cpu_rq(sg_cpu->cpu), boost, NULL)` clamps the `boost` value using the runqueue's aggregated uclamp min/max values. The `NULL` task pointer means it only considers the rq-level uclamp aggregation (which is the max of all runnable tasks' uclamp values), not any specific task. This is correct because iowait_boost is a per-CPU signal, not a per-task signal — it represents the aggregate I/O boost for the entire CPU.

After this fix, even when iowait_boost is at maximum, the resulting utilization value used for frequency selection will be clamped to the runqueue's `uclamp_max`. This makes iowait_boost consistent with `effective_cpu_util()`, which already applies the same clamping. The fix is minimal, correct, and complete — it closes the only code path in schedutil where utilization was not subject to uclamp constraints.

## Triggering Conditions

The following conditions must all be met to trigger the bug:

1. **CONFIG_UCLAMP_TASK must be enabled**: The kernel must be compiled with utilization clamping support. Without it, `uclamp_rq_util_with()` is a no-op that returns its input unchanged, and there is no uclamp_max restriction to bypass.

2. **Schedutil governor must be active**: The `sugov_iowait_apply()` function is only called when the schedutil cpufreq governor is managing frequency decisions. If another governor (e.g., performance, powersave, ondemand) is active, this code path is never reached.

3. **A real cpufreq driver must be registered**: The schedutil governor requires an actual cpufreq driver to register frequency domains and trigger utilization updates. Without a cpufreq driver, schedutil cannot be loaded or activated.

4. **A task must have uclamp_max set below maximum**: At least one runnable task on the target CPU must have its `uclamp_max` set to a value below `SCHED_CAPACITY_SCALE` (1024). This can be done via `sched_setattr()` or the `uclampset` utility (available in util-linux >= 2.37.2), or via cgroup uclamp settings.

5. **The task must perform I/O operations that trigger iowait**: The task must enter I/O wait (setting `p->in_iowait = 1`) and then wake up. Each wakeup from I/O wait sets the `SCHED_CPUFREQ_IOWAIT` flag during `enqueue_task_fair()`, which triggers `sugov_iowait_boost()`. Sustained I/O operations cause the boost to double on each consecutive wakeup until it reaches the CPU's maximum capacity.

6. **The CPU must not be idle for too long**: If the CPU goes idle for too long, `sugov_iowait_reset()` resets the boost to zero. The I/O operations must be frequent enough to keep the boost alive.

The bug is 100% reproducible given these conditions — it is not a race condition or timing-sensitive. Any I/O-heavy task with a uclamp_max cap on a schedutil-governed CPU will experience the bypass every time iowait_boost exceeds the uclamp_max value.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. Below are the detailed reasons and analysis.

### Why kSTEP cannot reproduce this bug

**1. No cpufreq driver in QEMU**: The bug exists in `sugov_iowait_apply()`, which is part of the schedutil cpufreq governor. The schedutil governor is only active when a cpufreq driver is registered with the kernel and schedutil is selected as the governor for at least one cpufreq policy. QEMU does not emulate cpufreq hardware — there is no cpufreq driver available in the QEMU virtual machine. Without a cpufreq driver, the entire schedutil code path (`sugov_update_single_freq()`, `sugov_update_single_perf()`, `sugov_update_shared()`, and consequently `sugov_iowait_apply()`) is never invoked. The function where the bug lives is simply dead code in a QEMU environment.

**2. No real I/O workloads**: The iowait_boost mechanism is triggered when a task wakes from I/O wait, specifically when `p->in_iowait` is set by the block layer or filesystem code when the task is sleeping waiting for I/O completion. kSTEP manages kernel-controlled tasks via `kstep_task_block()` and `kstep_task_wakeup()`, but these do not set the `in_iowait` flag — they simulate generic sleep/wakeup events, not I/O-specific waits. Without the `in_iowait` flag being set, the `SCHED_CPUFREQ_IOWAIT` flag is never passed to `sugov_iowait_boost()`, and the iowait_boost value remains zero.

**3. Observability of frequency decisions**: Even if the code path could somehow be reached, the bug's effect is a higher-than-expected CPU frequency request. In QEMU, there is no actual frequency scaling hardware, so the frequency "decision" has no observable effect. kSTEP's `kstep_cpu_set_freq()` sets the frequency scale factor but does not interact with the schedutil governor's output. There is no way to observe what frequency schedutil would have requested versus what it should have requested.

### What would need to be added to kSTEP

To reproduce this bug, kSTEP would need fundamental additions that go beyond minor API changes:

1. **A virtual cpufreq driver**: kSTEP would need to register a fake cpufreq driver that creates cpufreq policies, enabling the schedutil governor to be loaded and activated. This would require implementing the `cpufreq_driver` interface (`init`, `exit`, `verify`, `target_index` or `target`, `get`, `setpolicy`, etc.), registering frequency tables, and handling governor transitions. This is a substantial subsystem implementation, not a minor helper function.

2. **I/O wait simulation**: kSTEP would need an API like `kstep_task_iowait_block(p)` and `kstep_task_iowait_wakeup(p)` that sets `p->in_iowait = 1` before blocking and clears it after wakeup. While setting the flag itself is trivial, the enqueue path in `enqueue_task_fair()` checks `p->in_iowait` to decide whether to pass `SCHED_CPUFREQ_IOWAIT` to the cpufreq update hook. This chain must be intact.

3. **Governor output monitoring**: A mechanism to intercept or observe the utilization value (`sg_cpu->util`) computed by schedutil before it is converted to a frequency request. This could be a callback or a way to read `sg_cpu` structures, but these are private to `cpufreq_schedutil.c` and not exported.

### Alternative reproduction methods

The bug can be reproduced on real hardware (or a VM with cpufreq passthrough) as described in the patch cover letter:

1. Boot a Linux kernel v5.3 through v5.17 with `CONFIG_UCLAMP_TASK=y` and the schedutil governor active.
2. Run an I/O-heavy workload capped with uclamp_max: `uclampset -M 0 sysbench --test=cpu --threads=4 run`
3. Monitor CPU frequency via `/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq` or `turbostat`.
4. On the buggy kernel, the frequency will reach maximum (~3.1GHz on the test laptop). On the fixed kernel, the frequency will be capped according to the uclamp_max setting.

Alternatively, one could use `ftrace` to trace `sugov_iowait_apply()` and observe the `boost` and `sg_cpu->util` values before and after the function, comparing them against the rq's `uclamp[UCLAMP_MAX].value`.
