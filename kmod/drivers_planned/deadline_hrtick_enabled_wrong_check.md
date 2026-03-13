# Deadline: Missing HRTICK_DL Feature Check in set_next_task_dl()

**Commit:** `d16b7eb6f523eeac3cff13001ef2a59cd462aa73`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.12-rc4
**Buggy since:** v6.8-rc1 (introduced by `63ba8422f876` "sched/deadline: Introduce deadline servers")

## Bug Description

When a SCHED_DEADLINE task is picked to run, the scheduler calls `set_next_task_dl()` which, among other housekeeping, arms the high-resolution tick timer (`hrtick`) to preempt the task when its runtime budget expires. Before arming this timer, the code is supposed to check whether the `HRTICK_DL` scheduler feature is enabled via `hrtick_enabled_dl(rq)`. However, due to a regression introduced by the deadline servers patchset, the check was incorrectly changed to `hrtick_enabled(rq)`, which only verifies that the CPU is active and that the hrtimer subsystem is in high-resolution mode — it does **not** check the `HRTICK_DL` sched_feat flag.

The `HRTICK_DL` scheduler feature (accessed via `sched_feat(HRTICK_DL)`) defaults to `false` in the kernel's `features.h`. The purpose of this feature flag is to allow administrators to control whether deadline tasks receive fine-grained hrtick-based preemption for runtime enforcement. With the generic `hrtick_enabled(rq)` check, the hrtick timer gets armed for every deadline task pick on any system with high-resolution timers active, regardless of the `HRTICK_DL` setting.

The bug was introduced in commit `63ba8422f876` ("sched/deadline: Introduce deadline servers"), which refactored the deadline scheduling code to support two-level scheduling. During this refactoring, the `start_hrtick_dl()` call was moved from its original location in `set_next_task_dl()` to a new location, but the guarding condition was accidentally weakened from `hrtick_enabled_dl(rq)` to `hrtick_enabled(rq)`.

## Root Cause

Prior to the deadline servers patch (`63ba8422f876`), the `set_next_task_dl()` function contained the following correctly guarded hrtick arm:

```c
static void set_next_task_dl(struct rq *rq, struct task_struct *p, bool first)
{
    ...
    if (!first)
        return;

    if (hrtick_enabled_dl(rq))    /* Correct: checks sched_feat(HRTICK_DL) */
        start_hrtick_dl(rq, p);
    ...
}
```

The `hrtick_enabled_dl(rq)` function is defined in `kernel/sched/sched.h` as:

```c
static inline int hrtick_enabled_dl(struct rq *rq)
{
    if (!sched_feat(HRTICK_DL))
        return 0;
    return hrtick_enabled(rq);
}
```

It first checks the `HRTICK_DL` feature flag, then delegates to the generic `hrtick_enabled(rq)` which checks `cpu_active(cpu_of(rq))` and `hrtimer_is_hres_active(&rq->hrtick_timer)`.

When the deadline servers feature was introduced (`63ba8422f876`), the code was reorganized. The hrtick arm was initially moved from `set_next_task_dl()` into `pick_next_task_dl()`:

```c
static struct task_struct *pick_next_task_dl(struct rq *rq)
{
    ...
    p = pick_task_dl(rq);
    if (!p)
        return p;

    if (!p->dl_server)
        set_next_task_dl(rq, p, true);

    if (hrtick_enabled(rq))       /* Bug: lost sched_feat(HRTICK_DL) check */
        start_hrtick_dl(rq, &p->dl);

    return p;
}
```

The `hrtick_enabled_dl()` was replaced with `hrtick_enabled()`, dropping the `sched_feat(HRTICK_DL)` check. By the time of the fix commit (in the v6.12 cycle), the code had been further refactored so that the hrtick arm was back inside `set_next_task_dl()` but still using the wrong `hrtick_enabled(rq)` check at line 2388 of `deadline.c`:

```c
static void set_next_task_dl(struct rq *rq, struct task_struct *p, bool first)
{
    ...
    deadline_queue_push_tasks(rq);

    if (hrtick_enabled(rq))       /* Bug: should be hrtick_enabled_dl(rq) */
        start_hrtick_dl(rq, &p->dl);
}
```

Note that the other hrtick check in `task_tick_dl()` at line 2472 was never touched by the deadline servers patch and correctly uses `hrtick_enabled_dl(rq)`:

```c
if (hrtick_enabled_dl(rq) && queued && p->dl.runtime > 0 &&
    is_leftmost(&p->dl, &rq->dl))
    start_hrtick_dl(rq, &p->dl);
```

This means the inconsistency exists only in the `set_next_task_dl()` path (task pick time), not in the `task_tick_dl()` path (periodic tick time).

## Consequence

The primary consequence is that hrtick timers are armed for all SCHED_DEADLINE tasks on every task pick, even when the system administrator has explicitly disabled the `HRTICK_DL` feature (or when it is at its default disabled state). This has several impacts:

1. **Unnecessary timer interrupts:** Each time a SCHED_DEADLINE task is scheduled, `hrtick_start(rq, dl_se->runtime)` programs a high-resolution timer to fire after `dl_se->runtime` nanoseconds. This generates an extra hardware timer interrupt that would not occur if `HRTICK_DL` were properly checked. On systems with many SCHED_DEADLINE tasks or frequent task switches, this adds measurable interrupt overhead.

2. **Unexpected scheduling behavior:** The hrtick callback invokes `task_tick_dl()`, which can trigger preemption of the running DL task. System administrators who disable `HRTICK_DL` expect that DL tasks will only be preempted at regular tick boundaries or by higher-priority tasks, not by fine-grained hrtick timers. The unexpected hrticks break this expectation, potentially causing scheduling jitter and unpredictable latency spikes for deadline workloads.

3. **Inconsistent sched_feat behavior:** The `HRTICK_DL` feature flag becomes partially ineffective. While `task_tick_dl()` correctly respects the feature flag (not re-arming hrtick during ticks), `set_next_task_dl()` ignores it. This creates a confusing situation where toggling `HRTICK_DL` via `/sys/kernel/debug/sched/features` only partially affects behavior, making debugging and performance tuning difficult.

The bug does not cause crashes, data corruption, or security issues. It is a correctness and performance issue that affects the predictability of SCHED_DEADLINE scheduling.

## Fix Summary

The fix is a one-line change in `set_next_task_dl()` at line 2388 of `kernel/sched/deadline.c`:

```diff
-   if (hrtick_enabled(rq))
+   if (hrtick_enabled_dl(rq))
        start_hrtick_dl(rq, &p->dl);
```

This restores the correct behavior by replacing the generic `hrtick_enabled(rq)` with the DL-specific `hrtick_enabled_dl(rq)`, which first checks `sched_feat(HRTICK_DL)` before falling through to the generic hrtimer activity check. After this fix, the hrtick timer in `set_next_task_dl()` is only armed when `HRTICK_DL` is explicitly enabled and the hrtimer subsystem is active.

The fix is correct and complete because it restores the guard that existed before the deadline servers refactoring. It aligns the `set_next_task_dl()` hrtick check with the existing `task_tick_dl()` hrtick check (line 2472), which already correctly uses `hrtick_enabled_dl(rq)`. Both call sites now consistently respect the `HRTICK_DL` feature flag.

The fix was authored by Phil Auld (Red Hat), acked by Juri Lelli (Red Hat, SCHED_DEADLINE maintainer), and merged by Peter Zijlstra into the `sched/urgent` branch of the tip tree.

## Triggering Conditions

The bug triggers under the following conditions:

1. **CONFIG_SCHED_HRTICK=y:** The kernel must be compiled with hrtick support. This is enabled by default on most distribution kernels.

2. **High-resolution timers active:** The hrtimer subsystem must have switched to high-resolution mode (`hrtimer_is_hres_active()` returns true). This happens automatically on most modern hardware (x86 with LAPIC, ARM with arch timer) when `CONFIG_HIGH_RES_TIMERS=y`.

3. **HRTICK_DL is false (default):** The `HRTICK_DL` scheduler feature must be disabled, which is the default. If `HRTICK_DL` is enabled, both `hrtick_enabled(rq)` and `hrtick_enabled_dl(rq)` would return true, and the bug would be masked (hrtick would be armed either way, which is the intended behavior when the feature is on).

4. **A SCHED_DEADLINE task is scheduled:** A task running under the `SCHED_DEADLINE` policy must be picked as the next task to run. This causes `set_next_task_dl()` to be called with `first=true`. The task can be any regular SCHED_DEADLINE task (not a DL server entity, as DL server picks go through a different code path where `set_next_task_dl()` is not called for the served CFS task).

5. **The CPU must be active:** `cpu_active(cpu_of(rq))` must return true, which is the normal state for all online CPUs.

The bug is deterministic and 100% reproducible — it occurs on **every** SCHED_DEADLINE task pick on any affected kernel (v6.8 through v6.12-rc3) with CONFIG_SCHED_HRTICK=y and active hres timers. No race conditions, timing requirements, or special workload characteristics are needed beyond having a SCHED_DEADLINE task run on the system.

## Reproduce Strategy (kSTEP)

This bug can be reproduced in kSTEP by creating a SCHED_DEADLINE task using `sched_setattr_nocheck()` (already used by the existing `uclamp_inversion.c` driver) and observing whether the hrtick timer is incorrectly armed when HRTICK_DL is disabled.

### kSTEP Extensions Needed

No framework-level extensions are required. The existing `sched_setattr_nocheck()` function (available via `#include <uapi/linux/sched/types.h>`) can be called directly from the driver to set a task to SCHED_DEADLINE. The hrtick timer state can be inspected via `cpu_rq()` access from `internal.h`. One potential concern is whether QEMU's guest kernel has high-resolution timers active; if `hrtimer_is_hres_active()` returns false, the bug path won't be reached even on the buggy kernel. In that case, the driver should detect and report this as a skip/inconclusive rather than a pass.

### Step-by-Step Driver Plan

1. **Setup phase (in `setup()`):**
   - Configure at least 2 CPUs in QEMU (CPU 0 is reserved for the driver; the DL task will run on CPU 1).
   - Create a kstep task: `struct task_struct *dl_task = kstep_task_create();`
   - Pin the task to CPU 1: `kstep_task_pin(dl_task, 1, 1);`
   - Convert the task to SCHED_DEADLINE using `sched_setattr_nocheck()`:
     ```c
     struct sched_attr attr = {
         .size = sizeof(attr),
         .sched_policy = SCHED_DEADLINE,
         .sched_runtime  =  5000000,   /* 5ms runtime */
         .sched_deadline = 10000000,   /* 10ms deadline */
         .sched_period   = 10000000,   /* 10ms period */
     };
     sched_setattr_nocheck(dl_task, &attr);
     ```

2. **Verify preconditions (in `run()`):**
   - Use `KSYM_IMPORT` to access sched features or read from debugfs. Verify that `HRTICK_DL` is disabled (it is by default). Alternatively, write `NO_HRTICK_DL` to `/sys/kernel/debug/sched/features` via `kstep_write()` to ensure it is off.
   - Check if hres timers are active on CPU 1: access `cpu_rq(1)->hrtick_timer` and check `hrtimer_is_hres_active()`. If not active, report inconclusive (the bug path requires hres timers).

3. **Trigger the bug (in `run()`):**
   - Wake up the DL task: `kstep_task_wakeup(dl_task);`
   - Advance several ticks to let the scheduler pick the DL task on CPU 1: `kstep_tick_repeat(5);`
   - At this point, `set_next_task_dl()` should have been called for the DL task on CPU 1's runqueue.

4. **Observe the hrtick timer state (in `run()` or `on_tick_end()`):**
   - Access CPU 1's runqueue: `struct rq *rq1 = cpu_rq(1);`
   - Check if the hrtick timer is armed: `int armed = hrtimer_active(&rq1->hrtick_timer);`
   - Also verify the current task on CPU 1 is the DL task: `rq1->curr == dl_task`

5. **Pass/fail criteria:**
   - **Buggy kernel (FAIL):** The hrtick timer is armed (`hrtimer_active()` returns true) even though `HRTICK_DL` is disabled. This means `set_next_task_dl()` called `hrtick_start()` via the incorrect `hrtick_enabled(rq)` check.
     ```c
     if (armed && rq1->curr == dl_task)
         kstep_fail("hrtick armed despite HRTICK_DL=false");
     ```
   - **Fixed kernel (PASS):** The hrtick timer is NOT armed (`hrtimer_active()` returns false) because `hrtick_enabled_dl(rq)` correctly returns 0 when `HRTICK_DL` is off.
     ```c
     if (!armed && rq1->curr == dl_task)
         kstep_pass("hrtick not armed when HRTICK_DL=false");
     ```

6. **Additional validation (optional):**
   - Enable `HRTICK_DL` by writing `HRTICK_DL` to sched features debugfs.
   - Re-trigger scheduling of the DL task (block/wake cycle).
   - Verify hrtick IS armed now (confirming the mechanism works when enabled).
   - This proves the test logic is sound and the difference is specifically the feature flag check.

7. **Cleanup:**
   - Block or pause the DL task.
   - The driver should be deterministic since there are no race conditions — the bug is a simple wrong-function-call that fires on every DL task pick.

### Expected Behavior Summary

| Kernel Version | HRTICK_DL | hrtick_enabled(rq) | hrtick_enabled_dl(rq) | hrtick armed? | Result |
|---------------|-----------|--------------------|-----------------------|---------------|--------|
| Buggy (pre-fix) | false | true (hres active) | false (not checked!) | YES — armed via wrong check | FAIL |
| Fixed | false | true (hres active) | false | NO — correct | PASS |
| Either | true | true | true | YES — correctly armed | N/A (expected) |

### QEMU Configuration

- At least 2 CPUs (`-smp 2`)
- Standard memory (128M or more)
- No special topology required
- `CONFIG_SCHED_HRTICK=y` must be enabled in the kernel config
- `CONFIG_HIGH_RES_TIMERS=y` must be enabled (default on most configs)
