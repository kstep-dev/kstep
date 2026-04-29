# Uclamp: rq->uclamp_max Not Set on First Enqueue

**Commit:** `315c4f884800c45cb6bd8c90422fad554a8b9588`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.16-rc4
**Buggy since:** v5.9-rc1 (introduced by commit d81ae8aac85c "sched/uclamp: Fix initialization of struct uclamp_rq")

## Bug Description

The utilization clamping (uclamp) subsystem allows tasks to specify minimum and maximum utilization bounds via `sched_setattr()`. These per-task clamps are aggregated at the runqueue level in `struct uclamp_rq`, where `rq->uclamp[UCLAMP_MAX].value` tracks the effective maximum utilization clamp for all runnable tasks on that CPU. This value is critical for CPU frequency selection via schedutil — it determines the upper bound on the frequency the CPU should run at.

Commit `d81ae8aac85c` changed the initialization of `struct uclamp_rq` to properly set `rq->uclamp[UCLAMP_MAX].value` to `uclamp_none(UCLAMP_MAX)` which is `SCHED_CAPACITY_SCALE` (1024), rather than leaving it at zero. This was necessary because a companion patch introduced a static key (`sched_uclamp_used`) that skips `uclamp_rq_inc()`/`uclamp_rq_dec()` until userspace opts in, meaning the initial zero value would otherwise cap all CPUs to zero frequency. However, that same commit initialized `rq->uclamp_flags = 0`, omitting the `UCLAMP_FLAG_IDLE` flag.

This omission causes a subtle bug: when the first task with `uclamp_max < 1024` is enqueued on a runqueue after the `sched_uclamp_used` static key is enabled, the runqueue's `uclamp_max` value is not reset to match the task's lower clamp. Instead, it remains stuck at 1024. The task's `uclamp_max` constraint is effectively ignored during this initial window, allowing the CPU to run at a higher frequency than the task's clamp would permit.

The bug persists on a given runqueue from the moment the `sched_uclamp_used` static key is first enabled (i.e., the first `sched_setattr()` call with uclamp flags) until the runqueue transitions to idle and back — at which point the normal `uclamp_idle_value()` → `UCLAMP_FLAG_IDLE` → `uclamp_idle_reset()` cycle restores correct behavior.

## Root Cause

The root cause lies in the interaction between three mechanisms: (1) the initialization of `rq->uclamp_flags`, (2) the `uclamp_idle_reset()` function, and (3) the max-aggregation logic in `uclamp_rq_inc_id()`.

When `init_uclamp_rq()` is called during boot, it initializes each runqueue's uclamp state:

```c
static void __init init_uclamp_rq(struct rq *rq)
{
    enum uclamp_id clamp_id;
    struct uclamp_rq *uc_rq = rq->uclamp;

    for_each_clamp_id(clamp_id) {
        uc_rq[clamp_id] = (struct uclamp_rq) {
            .value = uclamp_none(clamp_id)
        };
    }

    rq->uclamp_flags = 0;  // BUG: should be UCLAMP_FLAG_IDLE
}
```

Here, `uclamp_none(UCLAMP_MAX)` returns `SCHED_CAPACITY_SCALE` (1024), so `rq->uclamp[UCLAMP_MAX].value = 1024`. The `uclamp_flags` is set to 0, meaning the `UCLAMP_FLAG_IDLE` bit is not set.

When a task is later enqueued, `uclamp_rq_inc_id()` is called for each clamp_id (UCLAMP_MIN and UCLAMP_MAX):

```c
static inline void uclamp_rq_inc_id(struct rq *rq, struct task_struct *p,
                                    enum uclamp_id clamp_id)
{
    struct uclamp_rq *uc_rq = &rq->uclamp[clamp_id];
    struct uclamp_se *uc_se = &p->uclamp[clamp_id];
    ...
    uclamp_idle_reset(rq, clamp_id, uc_se->value);
    ...
    if (uc_se->value > READ_ONCE(uc_rq->value))
        WRITE_ONCE(uc_rq->value, uc_se->value);
}
```

The function first calls `uclamp_idle_reset()`, which is supposed to reset the rq's clamp value to the task's value when the rq is transitioning out of idle:

```c
static inline void uclamp_idle_reset(struct rq *rq, enum uclamp_id clamp_id,
                                     unsigned int clamp_value)
{
    if (!(rq->uclamp_flags & UCLAMP_FLAG_IDLE))
        return;
    WRITE_ONCE(rq->uclamp[clamp_id].value, clamp_value);
}
```

Since `rq->uclamp_flags` is 0 (not `UCLAMP_FLAG_IDLE`), this function returns immediately without resetting the rq value. Next, the max-aggregation check `if (uc_se->value > READ_ONCE(uc_rq->value))` compares the task's clamp value against the rq's current value. For UCLAMP_MAX, if the task's `uclamp_max` is, say, 512, and the rq's value is still 1024 from initialization, the condition `512 > 1024` is false, so the rq value is not updated.

The result: `rq->uclamp[UCLAMP_MAX].value` remains at 1024 even though the only runnable task on that CPU has `uclamp_max = 512`.

Previously (before commit `d81ae8aac85c`), `rq->uclamp[UCLAMP_MAX].value` was initialized to 0 via `memset()`. So the comparison `uc_se->value > 0` was always true for any task with `uclamp_max > 0`, which effectively reset the rq's value on first enqueue. The `UCLAMP_FLAG_IDLE` was never needed for this initial case because the zero initialization accidentally made the max-aggregation update work. When `d81ae8aac85c` changed the initialization to 1024, this implicit behavior broke.

## Consequence

The primary consequence is incorrect CPU frequency scaling. When a task with `uclamp_max < 1024` is the first to enqueue on a CPU after the `sched_uclamp_used` static key is enabled, the runqueue's `uclamp_max` remains at 1024. The schedutil governor reads `rq->uclamp[UCLAMP_MAX].value` to cap the requested frequency, and with the value stuck at 1024, the frequency cap is ineffective. The CPU may run at a higher frequency than the task's `uclamp_max` intends, leading to unnecessary power consumption.

This is particularly impactful on battery-powered devices (phones, tablets, laptops) where uclamp is commonly used for power management. Background tasks that are deliberately clamped to low maximum utilization (e.g., `uclamp_max = 20%`) would run at full frequency during this initial window, defeating the purpose of the clamp entirely. On systems with Energy Aware Scheduling (EAS), the incorrect `uclamp_max` could also affect task placement decisions, since `uclamp_max` influences the perceived utilization used in energy calculations.

The bug window is bounded: it exists only from the first `sched_setattr()` call that enables the static key until each runqueue experiences its first idle→run→idle→run cycle. After the first full idle transition, `uclamp_idle_value()` sets `UCLAMP_FLAG_IDLE`, and subsequent enqueues work correctly. In practice, this window may be short (seconds to minutes after boot), but on lightly-loaded CPUs that rarely go idle and come back, the window could persist longer.

## Fix Summary

The fix is a single-line change in `init_uclamp_rq()`:

```c
-   rq->uclamp_flags = 0;
+   rq->uclamp_flags = UCLAMP_FLAG_IDLE;
```

By initializing `rq->uclamp_flags` with `UCLAMP_FLAG_IDLE` set, the kernel treats each runqueue as if it has just transitioned to idle at boot time. This ensures that when the first task is enqueued after the `sched_uclamp_used` static key is enabled, `uclamp_idle_reset()` sees the `UCLAMP_FLAG_IDLE` bit and resets `rq->uclamp[clamp_id].value` to the task's actual clamp value. The flag is then cleared by `uclamp_rq_inc()` after the per-clamp loop:

```c
if (rq->uclamp_flags & UCLAMP_FLAG_IDLE)
    rq->uclamp_flags &= ~UCLAMP_FLAG_IDLE;
```

This fix is correct because at boot, before any tasks are enqueued, the runqueue is conceptually idle — there are no runnable tasks. Setting `UCLAMP_FLAG_IDLE` accurately reflects this state. The fix ensures that the very first enqueue on any CPU correctly transitions the rq from "idle" to "active" with proper uclamp value propagation, regardless of whether the initial rq uclamp value was 0 or 1024.

The fix is also minimal and safe: it only affects the initialization path (`__init` function) and does not change any runtime logic. The `UCLAMP_FLAG_IDLE` flag is already part of the normal rq lifecycle; the fix simply ensures the rq starts in the correct initial state.

## Triggering Conditions

The bug requires the following conditions:

1. **CONFIG_UCLAMP_TASK=y**: The kernel must be compiled with utilization clamping support. This is standard on Android/mobile kernels and increasingly common on server kernels.

2. **First uclamp activation**: The `sched_uclamp_used` static key must be newly enabled. This happens when the first `sched_setattr()` call with `SCHED_FLAG_UTIL_CLAMP` is made. In kSTEP, calling `sched_setattr_nocheck()` with uclamp flags triggers this.

3. **Task with uclamp_max < SCHED_CAPACITY_SCALE (1024)**: The enqueued task must have a `uclamp_max` value less than 1024. If `uclamp_max == 1024`, the bug still occurs (the rq value is not reset), but it has no observable effect since 1024 is the maximum anyway.

4. **No prior idle cycle on the target CPU**: The target CPU's runqueue must not have gone through a complete enqueue→dequeue→idle→re-enqueue cycle since boot. This means the `UCLAMP_FLAG_IDLE` bit was never set by `uclamp_idle_value()` during a prior dequeue-to-idle.

5. **At least 2 CPUs**: Since CPU 0 is reserved by kSTEP for the driver, the test task must run on CPU 1 or higher.

The bug is deterministic and does not involve any race conditions. It is a pure initialization error that affects the first enqueue on each CPU. The probability of triggering it is 100% if the above conditions are met — the only question is whether the rq has already been "fixed" by a prior idle transition.

In a fresh kSTEP QEMU environment, since the `sched_uclamp_used` static key starts disabled and no userspace uclamp activity has occurred, the initial conditions are perfectly met. The first `sched_setattr_nocheck()` in the driver enables the key, and the first `kstep_task_wakeup()` triggers the buggy first-enqueue path.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP by creating a task with a restricted `uclamp_max`, enqueuing it on a CPU, and then checking whether the runqueue's `rq->uclamp[UCLAMP_MAX].value` correctly reflects the task's clamp value.

**Step 1: Create a task and set its uclamp_max to a value below 1024.**

Create a single CFS task using `kstep_task_create()`. Then use `sched_setattr_nocheck()` (as demonstrated in the existing `uclamp_inversion.c` driver) to set its `uclamp_max` to a distinctive value, such as 512 (50% of SCHED_CAPACITY_SCALE). This call will also enable the `sched_uclamp_used` static key if it is not already enabled.

```c
struct sched_attr attr = {
    .size = sizeof(attr),
    .sched_policy = SCHED_NORMAL,
    .sched_flags = SCHED_FLAG_UTIL_CLAMP,
    .sched_util_min = 0,
    .sched_util_max = 512,
};
sched_setattr_nocheck(task, &attr);
```

**Step 2: Pin the task to a specific CPU (not CPU 0).**

Use `kstep_task_pin(task, 1, 2)` to pin the task to CPU 1. This ensures the task will be enqueued on CPU 1's runqueue, whose `uclamp_flags` was initialized to 0 at boot.

**Step 3: Wake up the task.**

Call `kstep_task_wakeup(task)` to enqueue the task on CPU 1. This triggers `uclamp_rq_inc()` → `uclamp_rq_inc_id()` → `uclamp_idle_reset()` on CPU 1's runqueue.

**Step 4: Read rq->uclamp[UCLAMP_MAX].value from CPU 1's runqueue.**

After the task is enqueued, access the runqueue via `cpu_rq(1)` (available through kSTEP's `internal.h`) and read `rq->uclamp[UCLAMP_MAX].value`.

- **Buggy kernel (pre-fix):** `rq->uclamp[UCLAMP_MAX].value == 1024`. The `uclamp_idle_reset()` check fails because `UCLAMP_FLAG_IDLE` is not set, and the max-aggregation `512 > 1024` is false, so the rq value stays at its initialized 1024.
- **Fixed kernel (post-fix):** `rq->uclamp[UCLAMP_MAX].value == 512`. The `uclamp_idle_reset()` check succeeds because `UCLAMP_FLAG_IDLE` is set at init, so it writes the task's value (512) to the rq.

**Step 5: Apply pass/fail criteria.**

```c
struct rq *rq = cpu_rq(1);
unsigned int rq_uclamp_max = READ_ONCE(rq->uclamp[UCLAMP_MAX].value);
unsigned int task_uclamp_max = 512;

if (rq_uclamp_max == task_uclamp_max)
    kstep_pass("rq uclamp_max correctly set to %u", rq_uclamp_max);
else
    kstep_fail("rq uclamp_max is %u, expected %u", rq_uclamp_max, task_uclamp_max);
```

On the buggy kernel, `kstep_fail()` fires because `rq_uclamp_max` is 1024. On the fixed kernel, `kstep_pass()` fires because `rq_uclamp_max` is 512.

**Step 6: Version guard.**

The driver should be guarded with `#if LINUX_VERSION_CODE` checks. The buggy commit `d81ae8aac85c` was merged in v5.9-rc1, and the fix `315c4f884800` was merged in v5.16-rc4. The driver should only run on kernels in this range:

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
```

**Step 7: No callbacks needed.**

This bug is triggered purely by the enqueue path; no tick callbacks, softirq hooks, or load-balancing callbacks are required. The entire reproduce sequence is: create task → set uclamp → pin → wakeup → check rq state.

**Step 8: QEMU configuration.**

The QEMU instance should be configured with at least 2 CPUs (CPU 0 for the driver, CPU 1 for the test task). No special topology, memory, or capacity configuration is required.

**Step 9: Expected determinism.**

This test is fully deterministic. The bug is an initialization error, not a race condition. Every run on the buggy kernel will show `rq_uclamp_max == 1024`, and every run on the fixed kernel will show `rq_uclamp_max == 512`. There is no timing sensitivity or probabilistic component.

**Step 10: kSTEP API requirements.**

No extensions to kSTEP are needed. All required functionality is already available: `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()`, `cpu_rq()` (via internal.h), `sched_setattr_nocheck()` (kernel API accessible from modules), and `kstep_pass()`/`kstep_fail()` for result reporting. The existing `uclamp_inversion.c` driver serves as a working template for the uclamp setup code.
