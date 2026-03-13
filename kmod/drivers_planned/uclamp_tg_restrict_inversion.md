# Uclamp: Task-Group Restrict Effective Value Inversion

**Commit:** `0213b7083e81f4acd69db32cb72eb4e5f220329a`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.14-rc1
**Buggy since:** v5.14-rc1 (introduced by commit `0c18f2ecfcc2` "sched/uclamp: Fix wrong implementation of cpu.uclamp.min")

## Bug Description

The utilization clamping (uclamp) subsystem allows tasks and cgroups to set minimum (`uclamp_min`) and maximum (`uclamp_max`) utilization bounds. These bounds influence CPU frequency selection (via schedutil) and task placement decisions (via Energy Aware Scheduling). When a task belongs to a cgroup (task group), its effective uclamp values should be constrained to the allowed range defined by the task group. The function `uclamp_tg_restrict()` is responsible for computing this restricted effective clamp value for a given task.

Prior to commit `0c18f2ecfcc2`, `cpu.uclamp.min` acted as a limit (upper cap on the per-task `uclamp_min`). That commit changed the semantics so that `cpu.uclamp.min` acts as a protection (minimum floor), matching the cgroup-v2 resource distribution model described in `Documentation/admin-guide/cgroup-v2.rst`. After this change, if a task requests a `uclamp_min` lower than the task group's `uclamp_min`, the task group's value is used instead (protecting the task with at least that minimum utilization).

However, the implementation of `uclamp_tg_restrict()` after commit `0c18f2ecfcc2` handled `UCLAMP_MIN` and `UCLAMP_MAX` independently through a switch-case, without ensuring that the resulting effective `uclamp_min` never exceeds the effective `uclamp_max`. This independent handling creates corner cases where the effective min exceeds the effective max — a uclamp inversion that violates the fundamental invariant `effective_uclamp_min <= effective_uclamp_max`.

Additionally, the `uclamp_update_active_tasks()` function only updated the specific clamp ID (`UCLAMP_MIN` or `UCLAMP_MAX`) that actually changed in the task group. But since the fix couples min and max through a `clamp()` operation, changing the task group's `UCLAMP_MAX` could affect the effective `UCLAMP_MIN` of tasks (and vice versa), requiring both clamp IDs to be updated unconditionally.

## Root Cause

The root cause is in `uclamp_tg_restrict()` in `kernel/sched/core.c`. The buggy code handles each clamp ID independently:

```c
switch (clamp_id) {
case UCLAMP_MIN: {
    struct uclamp_se uc_min = task_group(p)->uclamp[clamp_id];
    if (uc_req.value < uc_min.value)
        return uc_min;
    break;
}
case UCLAMP_MAX: {
    struct uclamp_se uc_max = task_group(p)->uclamp[clamp_id];
    if (uc_req.value > uc_max.value)
        return uc_max;
    break;
}
...
}
```

For `UCLAMP_MIN`, the logic is: if the task's requested min is below the task group's min, raise it to the task group's min (protection semantics). Otherwise, keep the task's requested value. For `UCLAMP_MAX`, the logic is: if the task's requested max exceeds the task group's max, lower it to the task group's max (limit semantics). Otherwise, keep the task's requested value.

The problem is that these two operations are performed independently, without cross-checking the other clamp. Consider **CASE 1** from the commit message:
- Task: `uclamp_min = 60%`, `uclamp_max = 80%`
- Task group: `uclamp_min = 0%`, `uclamp_max = 50%`

When computing effective `UCLAMP_MIN`: task's 60% is not below tg's 0%, so effective min = 60%.
When computing effective `UCLAMP_MAX`: task's 80% exceeds tg's 50%, so effective max = 50%.
Result: effective min (60%) > effective max (50%) — **inversion**.

Consider **CASE 2**:
- Task: `uclamp_min = 0%`, `uclamp_max = 20%`
- Task group: `uclamp_min = 30%`, `uclamp_max = 50%`

When computing effective `UCLAMP_MIN`: task's 0% is below tg's 30%, so effective min = 30%.
When computing effective `UCLAMP_MAX`: task's 20% does not exceed tg's 50%, so effective max = 20%.
Result: effective min (30%) > effective max (20%) — **inversion**.

A second part of the bug is in `uclamp_update_active_tasks()`. This function is called from `cpu_util_update_eff()` when a task group's effective clamp values change. The buggy version takes a `clamps` bitmask and only updates the specific clamp IDs that changed:

```c
static inline void
uclamp_update_active_tasks(struct cgroup_subsys_state *css,
                           unsigned int clamps)
{
    ...
    while ((p = css_task_iter_next(&it))) {
        for_each_clamp_id(clamp_id) {
            if ((0x1 << clamp_id) & clamps)
                uclamp_update_active(p, clamp_id);
        }
    }
    ...
}
```

Similarly, `uclamp_update_active()` only updates a single `clamp_id` per call. But with the fixed `uclamp_tg_restrict()` using `clamp(value, tg_min, tg_max)`, changing only `UCLAMP_MAX` in the task group could cause the effective `UCLAMP_MIN` of a task to change (since the clamp constrains the task's min to be no greater than tg_max). If only `UCLAMP_MAX` is in the bitmask, the effective `UCLAMP_MIN` of already-enqueued tasks would not be updated, leaving stale values in the runqueue's bucket accounting.

## Consequence

The primary consequence is a uclamp inversion where a task's effective `uclamp_min` exceeds its effective `uclamp_max`. This has several downstream effects:

**Incorrect CPU frequency scaling:** The schedutil governor uses `uclamp_min` and `uclamp_max` to determine the target CPU frequency. When `uclamp_min > uclamp_max`, the frequency floor exceeds the frequency ceiling. Depending on how schedutil resolves this contradiction (e.g., by taking the max of the two), the CPU may run at a higher frequency than intended, wasting power. On mobile and embedded platforms (the primary users of uclamp), this directly impacts battery life. In the example from CASE 1 (effective min=60%, max=50%), a task that should be capped at 50% utilization could instead force the CPU to run at 60%.

**Incorrect Energy Aware Scheduling decisions:** EAS uses uclamp values when computing energy estimates for task placement. An inverted uclamp could cause tasks to be placed on larger/faster CPUs when they should be placed on smaller/more efficient ones, or vice versa. This compounds the power waste from incorrect frequency selection.

**Stale runqueue uclamp accounting:** The secondary bug (not updating both clamp IDs) means that when a cgroup's `UCLAMP_MAX` is raised, already-runnable tasks may retain a stale lower effective `UCLAMP_MIN` in the runqueue's uclamp bucket accounting. This can cause runqueue-level aggregation (`rq->uclamp[UCLAMP_MIN].value`) to be incorrect until those tasks are dequeued and re-enqueued.

This bug was reported by Xuewen Yan, who observed the corner case during testing of the new protection semantics for `cpu.uclamp.min`.

## Fix Summary

The fix addresses both parts of the bug:

**Part 1: Fix `uclamp_tg_restrict()`** — The switch-case that handled `UCLAMP_MIN` and `UCLAMP_MAX` independently is replaced with a single `clamp()` operation that constrains the task's requested value to the range `[tg_min, tg_max]`:

```c
tg_min = task_group(p)->uclamp[UCLAMP_MIN].value;
tg_max = task_group(p)->uclamp[UCLAMP_MAX].value;
value = uc_req.value;
value = clamp(value, tg_min, tg_max);
uclamp_se_set(&uc_req, value, false);
```

This ensures that regardless of which `clamp_id` is being computed, the result is always bounded by both the task group's min and max. For CASE 1 (task min=60%, tg min=0%, tg max=50%), the effective min becomes `clamp(60%, 0%, 50%) = 50%`. For the max, `clamp(80%, 0%, 50%) = 50%`. Both are 50%, maintaining the invariant. For CASE 2 (task min=0%, tg min=30%, tg max=50%), effective min = `clamp(0%, 30%, 50%) = 30%`, effective max = `clamp(20%, 30%, 50%) = 30%`. Both are 30%, maintaining the invariant.

**Part 2: Fix `uclamp_update_active()` and `uclamp_update_active_tasks()`** — The `clamp_id` parameter is removed from `uclamp_update_active()`, and the function now loops over all clamp IDs using `for_each_clamp_id()`. Similarly, `uclamp_update_active_tasks()` drops the `clamps` bitmask parameter and unconditionally calls `uclamp_update_active()` for each task. The call site in `cpu_util_update_eff()` is simplified to just `uclamp_update_active_tasks(css)`. This ensures that when any task group clamp value changes, both the min and max effective values of all runnable tasks in that group are recalculated and their runqueue bucket accounting is updated accordingly.

The fix is correct because the `clamp()` operation guarantees `tg_min <= result <= tg_max` for any input. Combined with the existing `eff[UCLAMP_MIN] = min(eff[UCLAMP_MIN], eff[UCLAMP_MAX])` in `cpu_util_update_eff()` that ensures `tg_min <= tg_max`, the effective clamp values for any task always satisfy `effective_min <= effective_max`.

## Triggering Conditions

The bug requires the following conditions:

1. **CONFIG_UCLAMP_TASK and CONFIG_UCLAMP_TASK_GROUP enabled:** Both must be enabled in the kernel configuration to have per-task uclamp and cgroup-based uclamp task group restrictions.

2. **A non-root, non-autogroup task group (cgroup):** The task must be in a task group where `uclamp_tg_restrict()` actually applies the task group's clamp values. Tasks in the root task group or autogroups bypass the restriction logic.

3. **An inversion between task-level and task-group-level uclamp values:** Specifically, one of these scenarios:
   - **CASE 1:** The task's `uclamp_min` is higher than the task group's `uclamp_max`. For example, task `uclamp_min = 60%`, task group `uclamp_max = 50%`. The task group's `uclamp_min` must be lower than the task's min (e.g., 0%) so that the protection logic does not override it.
   - **CASE 2:** The task group's `uclamp_min` is higher than the task's `uclamp_max`. For example, task group `uclamp_min = 30%`, task `uclamp_max = 20%`. The task group's `uclamp_max` must be higher than the task's max (e.g., 50%) so that the limit logic does not override it.

4. **The task must be enqueued (runnable):** The uclamp effective values are used during enqueue/dequeue and for frequency/energy decisions. The inversion is visible whenever the effective uclamp value is computed, which happens at enqueue time and during tick processing for frequency selection.

5. **At least 1 CPU:** No special topology requirements. The bug is purely a logic error in the clamp computation, not a race condition or concurrency issue. It triggers deterministically whenever the above value conditions are met.

The bug is 100% deterministic and does not depend on timing, CPU count, or any race condition. It is triggered every time a task with the specific uclamp values is enqueued to a runqueue while belonging to a task group with conflicting uclamp bounds.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP using a straightforward approach that creates a cgroup with specific uclamp bounds and a task with conflicting per-task uclamp values. An existing driver `uclamp_inversion.c` already targets this commit.

**Step-by-step plan:**

1. **Create a task:** Use `kstep_task_create()` to create a single CFS task. This task will be the subject whose effective uclamp values we observe.

2. **Create a cgroup with specific uclamp bounds:** Use `kstep_cgroup_create("test_tg")` to create a child cgroup. Then set its uclamp bounds to create the inversion condition:
   - For CASE 1: `kstep_cgroup_write("test_tg", "cpu.uclamp.min", "0.00")` and `kstep_cgroup_write("test_tg", "cpu.uclamp.max", "50.00")` (tg_min=0%, tg_max=50%).
   - For CASE 2: `kstep_cgroup_write("test_tg", "cpu.uclamp.min", "30.00")` and `kstep_cgroup_write("test_tg", "cpu.uclamp.max", "50.00")` (tg_min=30%, tg_max=50%).

3. **Add the task to the cgroup:** Use `kstep_cgroup_add_task("test_tg", task->pid)`.

4. **Set per-task uclamp values using `sched_setattr_nocheck()`:** Since kSTEP runs in kernel space, call `sched_setattr_nocheck()` (imported via `KSYM_IMPORT`) with `SCHED_FLAG_UTIL_CLAMP`:
   - For CASE 1: Set task `uclamp_min = 614` (60% of 1024) and `uclamp_max = 819` (80% of 1024).
   - For CASE 2: Set task `uclamp_min = 0` and `uclamp_max = 205` (20% of 1024).

5. **Wake up the task:** Call `kstep_task_wakeup(task)` to enqueue the task on a runqueue. This triggers `uclamp_rq_inc()` → `uclamp_rq_inc_id()`, which calls `uclamp_eff_get()` → `uclamp_tg_restrict()` to compute the effective uclamp values.

6. **Read the task's effective (active) uclamp values:** After the task is enqueued, read `task->uclamp[UCLAMP_MIN].value` and `task->uclamp[UCLAMP_MAX].value` (these are the effective, back-annotated values stored when the task is active on a runqueue).

7. **Check for inversion (pass/fail criteria):**
   - Read `unsigned int eff_min = task->uclamp[UCLAMP_MIN].value` and `unsigned int eff_max = task->uclamp[UCLAMP_MAX].value`.
   - **Buggy kernel:** `eff_min > eff_max` (e.g., CASE 1: min=614, max=512; CASE 2: min=307, max=205). Call `kstep_fail("uclamp inversion: min=%u > max=%u", eff_min, eff_max)`.
   - **Fixed kernel:** `eff_min <= eff_max` (e.g., CASE 1: min=512, max=512; CASE 2: min=307, max=307). Call `kstep_pass("uclamp correct: min=%u <= max=%u", eff_min, eff_max)`.

8. **Additionally test the `uclamp_update_active_tasks()` fix:** After the task is running with the initial inversion, change the task group's `UCLAMP_MAX` to a higher value (e.g., `kstep_cgroup_write("test_tg", "cpu.uclamp.max", "70.00")`). On the buggy kernel, only `UCLAMP_MAX` would be updated for the active task, leaving `UCLAMP_MIN` stale. On the fixed kernel, both are updated. Read the effective values again and verify consistency.

9. **QEMU configuration:** 2 CPUs are sufficient (CPU 0 reserved for the driver, task runs on CPU 1). Pin the task to CPU 1 with `kstep_task_pin(task, 1, 2)`. No special topology or memory configuration needed.

10. **Kernel configuration:** Ensure `CONFIG_UCLAMP_TASK=y` and `CONFIG_UCLAMP_TASK_GROUP=y` are enabled. These are typically enabled on ARM/mobile kernel configs. The `sched_uclamp_used` static key must be enabled, which happens automatically when `sched_setattr_nocheck()` is called with uclamp flags.

**Expected behavior:**
- **Buggy kernel (v5.14-rc1 before fix):** The effective `uclamp_min` exceeds `uclamp_max`, and the inversion is observable in `task->uclamp[UCLAMP_MIN].value > task->uclamp[UCLAMP_MAX].value`.
- **Fixed kernel (v5.14-rc1 with fix):** The effective values are always consistent: `uclamp_min <= uclamp_max`, with values clamped to the task group's `[tg_min, tg_max]` range.
