# Uclamp: Reset-on-fork from RT Incorrectly Sets uclamp.min to Max

**Commit:** `eaf5a92ebde5bca3bb2565616115bd6d579486cd`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.7-rc3
**Buggy since:** v5.3-rc1 (introduced by commit `1a00d999971c` "sched/uclamp: Set default clamps for RT tasks")

## Bug Description

The utilization clamping (uclamp) subsystem allows constraining the minimum and maximum utilization of tasks, which influences CPU frequency selection via schedutil. By default, RT (real-time) tasks are given a `uclamp.min` value of 1024 (i.e., `SCHED_CAPACITY_SCALE`, or 100% boost), meaning they always request the maximum CPU frequency. FAIR/CFS tasks, on the other hand, start with `uclamp.min = 0` (no boost) so their frequency is determined by their actual utilization demand.

When a task has the `sched_reset_on_fork` flag set and it forks a child, the child's scheduling policy is demoted to `SCHED_NORMAL` if the parent was a privileged RT or FIFO task. This reset-on-fork mechanism prevents unprivileged child processes from inheriting elevated scheduling priorities. As part of this reset, the child's uclamp values should also be reset to defaults appropriate for a `SCHED_NORMAL` task.

The bug occurs in `uclamp_fork()`, which is called during `sched_fork()` as part of the `copy_process()` fork path. When `sched_reset_on_fork` is set, `uclamp_fork()` resets the uclamp request values. However, it also contains logic inherited from `__setscheduler_uclamp()` that checks `rt_task(p)` and, if the task is still RT at the time `uclamp_fork()` runs, sets `uclamp.min` to 1024 (max boost). The critical problem is that `uclamp_fork()` is called *before* the policy demotion to `SCHED_NORMAL` happens. So the child task, which will ultimately run as `SCHED_NORMAL`, ends up with `uclamp.min = 1024` — effectively requesting maximum CPU frequency as if it were an RT task.

This means any `SCHED_NORMAL` child process forked from an RT parent with `sched_reset_on_fork` gets permanently boosted to maximum frequency, which is both incorrect from a scheduling perspective and wasteful of power/energy.

## Root Cause

The root cause is a temporal ordering mismatch between `uclamp_fork()` and the policy reset in `sched_fork()`.

In the `sched_fork()` function (called from `copy_process()`), the sequence of operations is:

1. `uclamp_fork(p)` is called — this resets uclamp values for the child task.
2. Then, if `sched_reset_on_fork` is set and the task has an RT policy, the policy is lowered: `p->policy = SCHED_NORMAL` and `p->normal_prio = p->static_prio`.

Inside `uclamp_fork()`, when `sched_reset_on_fork` is set, the buggy code iterates over each clamp ID and does:

```c
for_each_clamp_id(clamp_id) {
    unsigned int clamp_value = uclamp_none(clamp_id);

    /* By default, RT tasks always get 100% boost */
    if (unlikely(rt_task(p) && clamp_id == UCLAMP_MIN))
        clamp_value = uclamp_none(UCLAMP_MAX);

    uclamp_se_set(&p->uclamp_req[clamp_id], clamp_value, false);
}
```

`uclamp_none(UCLAMP_MIN)` returns 0 (no boost), and `uclamp_none(UCLAMP_MAX)` returns 1024 (max capacity). For `UCLAMP_MIN`, the code checks `rt_task(p)`, and since the task's policy has *not yet been changed* to `SCHED_NORMAL` at this point, `rt_task(p)` returns true if the parent was RT. This causes `clamp_value` to be set to 1024 instead of 0.

After `uclamp_fork()` returns, `sched_fork()` demotes the policy to `SCHED_NORMAL`, but the `uclamp_req[UCLAMP_MIN]` value has already been incorrectly set to 1024 and is never corrected. The child task thus runs as `SCHED_NORMAL` with `uclamp.min = 1024`.

The logic for setting RT tasks' `uclamp.min` to max was copied from `__setscheduler_uclamp()`, where it makes sense because that function is called when a task's scheduling class is being set (and the policy is already determined). In `uclamp_fork()` with `sched_reset_on_fork`, the policy is *about to be changed*, so checking the current (soon-to-be-stale) policy is incorrect.

## Consequence

The observable consequence is that child processes forked from RT tasks with `sched_reset_on_fork` run with an incorrect `uclamp.min` value of 1024 (100% boost), even though they are demoted to `SCHED_NORMAL`. This has the following impacts:

1. **Excessive CPU frequency**: The schedutil governor uses `uclamp.min` to determine the minimum frequency for a CPU. A `SCHED_NORMAL` task with `uclamp.min = 1024` will cause the CPU to run at maximum frequency whenever the task is runnable, regardless of its actual utilization. This leads to significantly increased power consumption and thermal output, which is particularly damaging on battery-powered devices (mobile phones, laptops, embedded systems).

2. **Incorrect Energy-Aware Scheduling**: Energy-Aware Scheduling (EAS) uses uclamp values to determine task placement across asymmetric CPU topologies (e.g., big.LITTLE). A task with `uclamp.min = 1024` would be placed on the highest-performance CPU cores even if it doesn't need them, defeating the purpose of EAS and wasting energy.

3. **Silent regression**: There is no crash, warning, or visible error. The bug manifests as degraded battery life and thermal behavior, making it difficult to diagnose. It was reported by Chitti Babu Theegala at Qualcomm (codeaurora.org), likely after observing unexpected frequency behavior on Android devices.

## Fix Summary

The fix is straightforward: remove the `rt_task(p)` check and the associated `uclamp.min = 1024` override from `uclamp_fork()`. When `sched_reset_on_fork` is set, the uclamp values should always be reset to their class-default values using `uclamp_none(clamp_id)`, which returns 0 for `UCLAMP_MIN` and 1024 for `UCLAMP_MAX`.

The fixed code simplifies the loop to:

```c
for_each_clamp_id(clamp_id) {
    uclamp_se_set(&p->uclamp_req[clamp_id],
                  uclamp_none(clamp_id), false);
}
```

This is correct because, in the `sched_reset_on_fork` path, the child task will *always* be demoted to `SCHED_NORMAL` (if it was RT/FIFO), so setting RT-specific uclamp defaults is never appropriate here. If the child task is later promoted back to RT via `sched_setscheduler()`, the `__setscheduler_uclamp()` function will correctly set `uclamp.min = 1024` at that time.

The fix also removes the unnecessary local variable `clamp_value`, as suggested during review by Doug Anderson and Patrick Bellasi, making the code cleaner and more concise.

## Triggering Conditions

The bug is triggered under the following precise conditions:

1. **CONFIG_UCLAMP_TASK must be enabled**: The uclamp subsystem must be compiled into the kernel. This is common on Android kernels and ARM-based systems but not universally enabled.

2. **An RT parent task with `sched_reset_on_fork` set**: A task must be running with an RT scheduling policy (`SCHED_FIFO` or `SCHED_RR`) and must have the `sched_reset_on_fork` flag set. This flag is typically set via `sched_setattr()` with `SCHED_FLAG_RESET_ON_FORK`, or by passing `SCHED_RESET_ON_FORK` ORed into the policy to `sched_setscheduler()`.

3. **The RT task forks a child**: The RT task must call `fork()` or `clone()`. The child process goes through `copy_process()` → `sched_fork()` → `uclamp_fork()`.

4. **No explicit user-defined uclamp values on the child**: If the parent had user-defined uclamp values (set via `sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP_MIN`), the reset-on-fork path would still overwrite them with the buggy max-boost value.

The bug is 100% deterministic — it occurs every time these conditions are met. There is no race condition or timing dependency involved. Any RT task that forks with `sched_reset_on_fork` will produce a child with incorrect `uclamp.min = 1024`.

The number of CPUs and topology are irrelevant to triggering the bug itself, though the consequences (frequency boosting, task placement) are more visible on systems with asymmetric CPU topologies (big.LITTLE) and the schedutil frequency governor.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reason:

1. **KERNEL VERSION TOO OLD**: The fix commit `eaf5a92ebde5bca3bb2565616115bd6d579486cd` was merged into **v5.7-rc3**. The bug was introduced in v5.3-rc1 and existed through v5.7-rc2. kSTEP supports Linux **v5.15 and newer only**. Since the fix predates v5.15 by over a year, any v5.15+ kernel already includes this fix, and the buggy code path no longer exists.

2. **Cannot check out a buggy kernel version**: The standard kSTEP workflow (`./checkout_linux.py [hash]~1 [name]_buggy`) would check out a kernel from the v5.7-rc2 era, which kSTEP cannot build or run. The kSTEP build system, QEMU configuration, and kernel module framework are designed for v5.15+ kernels and rely on APIs, Kconfig options, and build infrastructure that may not exist in v5.7.

3. **The bug is purely a logic error in `uclamp_fork()`**: Were the kernel version supported, this bug would otherwise be straightforward to reproduce with kSTEP. The approach would be:
   - Create an RT task with `kstep_task_fifo(p)`.
   - Set `p->sched_reset_on_fork = 1` via direct struct access.
   - Fork the task with `kstep_task_fork(p, 1)`.
   - Read the child's `p->uclamp_req[UCLAMP_MIN].value`.
   - On a buggy kernel, this value would be 1024; on a fixed kernel, it would be 0.

4. **No kSTEP extensions would help**: The limitation is fundamentally about kernel version compatibility, not missing kSTEP features. No amount of API additions or framework changes can make kSTEP run on a pre-v5.15 kernel.

5. **Alternative reproduction outside kSTEP**: This bug can be trivially reproduced in userspace on an affected kernel (v5.3 through v5.7-rc2) with the following approach:
   - Write a small C program that sets itself to `SCHED_FIFO` with `SCHED_RESET_ON_FORK` via `sched_setattr()`.
   - Fork a child process.
   - In the child, read `/proc/self/sched` or use `sched_getattr()` to inspect the scheduling policy (should be `SCHED_NORMAL`).
   - Read the child's uclamp values from `/proc/<pid>/sched` (look for `uclamp.min`).
   - On a buggy kernel, `uclamp.min` will be 1024; on a fixed kernel, it will be 0.
   - Alternatively, monitor CPU frequency via `/sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq` while the child runs a light workload — on a buggy kernel, the frequency will be pinned to max.
