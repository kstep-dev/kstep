# EEVDF: SCHED_IDLE Entity Retains Slice Protection Against Non-Idle Wakeup Preemption

**Commit:** `f553741ac8c0e467a3b873e305f34b902e50b86d`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.15-rc1
**Buggy since:** v6.6-rc1 (introduced by commit `63304558ba5d` "sched/eevdf: Curb wakeup-preemption")

## Bug Description

The EEVDF scheduler's "RUN_TO_PARITY" feature, introduced in commit `63304558ba5d`, allows a currently running task to continue executing without being preempted until it either becomes ineligible or exhausts its current time slice. This is implemented via a "slice protection" mechanism: when a task is picked to run in `set_next_entity()`, its `vlag` field is set equal to its `deadline` field (`se->vlag = se->deadline`). Later, in `pick_eevdf()`, if the current entity's `vlag` equals its `deadline`, the scheduler returns it immediately without considering other eligible entities — effectively granting it immunity from preemption for the duration of its slice.

The bug is that this slice protection is applied uniformly to all CFS entities, including those running under `SCHED_IDLE` policy. When a `SCHED_IDLE` task is the current task and has slice protection active, and a `SCHED_NORMAL` (or other non-idle) task wakes up, the `check_preempt_wakeup_fair()` function correctly identifies that a non-idle entity should preempt an idle entity and jumps to the `preempt` label. However, when `pick_eevdf()` is subsequently called during the actual task pick, the idle entity's slice protection (`vlag == deadline`) causes `pick_eevdf()` to return the idle entity as the preferred pick, effectively defeating the preemption decision. The reschedule flag is set, but the idle task gets re-picked because its slice protection is still active.

This means that a `SCHED_IDLE` task that has just started running (and thus has a fresh slice) can block a `SCHED_NORMAL` task from running for up to the full duration of its time slice (typically 3ms with default settings). This is fundamentally wrong: `SCHED_IDLE` tasks should yield to `SCHED_NORMAL` tasks immediately upon wakeup, as the entire purpose of the `SCHED_IDLE` policy is to run only when no other non-idle work is available.

## Root Cause

The root cause is a missing interaction between two independently correct features: the idle-vs-non-idle preemption logic in `check_preempt_wakeup_fair()` and the RUN_TO_PARITY slice protection in `pick_eevdf()`.

When a task is selected to run, `set_next_entity()` activates slice protection by setting `se->vlag = se->deadline`. This is the "HACK" comment in the code — the `vlag` field is repurposed (it is not used between pick and dequeue) to stash the deadline at pick time. In `pick_eevdf()`, the check `if (sched_feat(RUN_TO_PARITY) && curr && curr->vlag == curr->deadline)` detects that the current entity was just picked and hasn't consumed a new slice yet. If true, `pick_eevdf()` returns `curr` immediately without performing the EEVDF eligible-deadline search, guaranteeing the current task continues running.

In `check_preempt_wakeup_fair()`, when a non-idle entity wakes up and the current entity is idle, the code at lines ~8794 (pre-fix) simply does `goto preempt`, which calls `resched_curr_lazy(rq)` to set TIF_NEED_RESCHED. This correctly triggers a reschedule. However, the idle entity's slice protection (`se->vlag == se->deadline`) remains intact. When the scheduler subsequently calls `pick_eevdf()` to select the next task, it sees the still-active slice protection and returns the idle entity as `curr`, negating the preemption.

The critical missing step was: when deciding to preempt an idle entity in favor of a non-idle entity, the idle entity's slice protection must also be cancelled. Without cancellation, the `pick_eevdf()` shortcut returns the idle entity before the EEVDF algorithm can even consider the woken non-idle entity. The `resched_curr` call becomes effectively useless because the same idle task gets re-selected.

The existing code did handle a somewhat analogous case for `do_preempt_short()` — when a waking entity has a shorter slice and is eligible, it would cancel slice protection via `se->vlag = se->deadline + 1`. But this logic was gated behind `se->vlag == se->deadline` as an additional condition (`if (do_preempt_short(cfs_rq, pse, se) && se->vlag == se->deadline)`), and crucially, the idle entity preemption path at `if (cse_is_idle && !pse_is_idle) goto preempt` had no such cancellation at all.

## Consequence

The observable impact is a significant scheduling latency increase for `SCHED_NORMAL` tasks when they share a CPU with `SCHED_IDLE` tasks. Specifically, when a `SCHED_IDLE` task sleeps for a period (e.g., 1 second) and then wakes up and starts executing, it receives a full fresh time slice with slice protection active. If a `SCHED_NORMAL` task wakes up during this window, it must wait for the `SCHED_IDLE` task to either exhaust its slice or become ineligible before it can run.

The commit message provides a concrete example: a `SCHED_IDLE` task that sleeps for 1 second then runs for 3ms, sharing a CPU with a cyclictest workload. The maximum measured latency was 3ms — the full slice duration of the idle task — because the idle task's slice protection prevented the cyclictest (normal priority) task from preempting it. This is a latency regression that directly contradicts the design intent of `SCHED_IDLE`, which should have near-zero impact on normal-priority workloads.

This bug does not cause crashes, hangs, or data corruption. It is a scheduling quality/latency bug that violates the expected priority semantics: `SCHED_IDLE` tasks should be completely transparent to `SCHED_NORMAL` tasks in terms of scheduling latency. Any real-time or latency-sensitive application sharing CPUs with `SCHED_IDLE` background work would experience unexpected latency spikes equal to the idle task's slice duration.

## Fix Summary

The fix introduces three helper functions to encapsulate the slice protection mechanism: `set_protect_slice(se)` (sets `se->vlag = se->deadline`), `protect_slice(se)` (returns `se->vlag == se->deadline`), and `cancel_protect_slice(se)` (sets `se->vlag = se->deadline + 1` if protection is active). These replace the raw `se->vlag == se->deadline` checks and assignments throughout `fair.c`.

The key behavioral change is in `check_preempt_wakeup_fair()`: when a non-idle entity is about to preempt an idle entity (`cse_is_idle && !pse_is_idle`), the fix adds `cancel_protect_slice(se)` before jumping to the `preempt` label. This ensures that when `pick_eevdf()` is called during the subsequent reschedule, the idle entity's slice protection is no longer active, so the EEVDF algorithm proceeds to evaluate all eligible entities and correctly picks the non-idle waking entity (which will have an earlier deadline or better eligibility).

The fix also cleans up the `do_preempt_short` path: the old code was `if (do_preempt_short(cfs_rq, pse, se) && se->vlag == se->deadline) se->vlag = se->deadline + 1`, which combined the short-preempt check with protection detection in one conditional. The new code is `if (do_preempt_short(cfs_rq, pse, se)) cancel_protect_slice(se)`, which is cleaner because `cancel_protect_slice()` internally checks whether protection is active before modifying `vlag`. This is functionally equivalent for the short-preempt case but more maintainable. Peter Zijlstra added the helper functions on top of the original patch to improve code clarity.

## Triggering Conditions

The bug requires the following conditions to trigger:

- **Kernel version**: v6.6-rc1 through v6.14 (any kernel with commit `63304558ba5d` but without commit `f553741ac8c0`).
- **Scheduler feature**: `RUN_TO_PARITY` must be enabled (it is enabled by default).
- **CPU sharing**: At least one `SCHED_IDLE` task and one `SCHED_NORMAL` task must be runnable on the same CPU (or at least the same CFS run queue in a cgroup hierarchy).
- **Timing**: The `SCHED_IDLE` task must be the currently running task with active slice protection (i.e., it was recently picked by `set_next_entity()` and has not yet consumed enough runtime to get a new slice or become ineligible). This means the idle task must have recently started or resumed running.
- **Wakeup**: The `SCHED_NORMAL` task must wake up while the `SCHED_IDLE` task is running with protection active. The wake-up triggers `check_preempt_wakeup_fair()`, which sets TIF_NEED_RESCHED, but the subsequent `pick_eevdf()` re-selects the protected idle entity.

The scenario is straightforward to trigger: a `SCHED_IDLE` task that periodically sleeps and then computes, sharing a CPU with a latency-sensitive `SCHED_NORMAL` task that also periodically wakes. The idle task's compute burst must be long enough to overlap with the normal task's wakeup while slice protection is still active (within the first few milliseconds of the idle task's burst). The longer the idle task's compute burst relative to its slice, the higher the probability of hitting this window.

No special kernel configuration, cgroup setup, or multi-CPU topology is required beyond having at least 2 CPUs (one for the driver, one for the test). No race conditions or specific orderings beyond the basic wakeup-during-protected-execution window are needed. The bug is deterministic given the right timing.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP. The strategy involves creating a `SCHED_IDLE` task and a `SCHED_NORMAL` task on the same CPU, then observing that the normal task fails to preempt the idle task when the idle task has slice protection active.

### Step-by-step plan:

1. **Task creation**: Create two tasks: `idle_task` and `normal_task`. Pin both to CPU 1 (CPU 0 is reserved for the driver).

2. **Set scheduling policies**: Set `idle_task` to `SCHED_IDLE` using `sched_setattr_nocheck()` with `.sched_policy = SCHED_IDLE`. Leave `normal_task` as `SCHED_NORMAL` (the default from `kstep_task_create()`).

3. **Initial state**: Wake up `idle_task` only. Let it run for a few ticks so it becomes the current task on CPU 1 and gets slice protection via `set_next_entity()`.

4. **Trigger the bug**: While `idle_task` is running with active slice protection, wake up `normal_task` on the same CPU. This triggers `check_preempt_wakeup_fair()` which detects the idle-vs-non-idle case and sets TIF_NEED_RESCHED.

5. **Observe**: After the wakeup, advance one or more ticks and check which task is `curr` on CPU 1. On a buggy kernel, `idle_task` will remain the current task because `pick_eevdf()` returns it due to slice protection. On a fixed kernel, `normal_task` will be scheduled immediately.

6. **Detection**: Use `kstep_output_curr_task()` in the `on_tick_begin` callback to observe which task is running on CPU 1 after the wakeup. Alternatively, read `cpu_rq(1)->curr` directly via internal.h and compare its PID to `normal_task->pid`.

7. **Pass/fail criteria**:
   - Read the sched_entity of the idle task (`idle_task->se`) and verify its `vlag` and `deadline` fields.
   - After waking `normal_task`, check if `cpu_rq(1)->curr == normal_task` within 1-2 ticks.
   - On **buggy kernel**: `cpu_rq(1)->curr` will still be `idle_task` (its `se.vlag == se.deadline` remains, meaning slice protection is intact). **kstep_fail()**.
   - On **fixed kernel**: `cpu_rq(1)->curr` will be `normal_task` (slice protection was cancelled by `cancel_protect_slice()`, and `pick_eevdf()` selected the non-idle entity). **kstep_pass()**.

### Implementation details:

```c
#include "driver.h"
#include "internal.h"
#include <uapi/linux/sched/types.h>

static struct task_struct *idle_task, *normal_task;

static void setup(void) {
    idle_task = kstep_task_create();
    normal_task = kstep_task_create();
}

static void run(void) {
    /* Pin both to CPU 1 */
    kstep_task_pin(idle_task, 1, 1);
    kstep_task_pin(normal_task, 1, 1);

    /* Set idle_task to SCHED_IDLE */
    struct sched_attr attr = {
        .size = sizeof(struct sched_attr),
        .sched_policy = SCHED_IDLE,
    };
    sched_setattr_nocheck(idle_task, &attr);

    /* Wake idle_task — it becomes curr on CPU 1 with slice protection */
    kstep_task_wakeup(idle_task);
    kstep_tick_repeat(2);  /* Let it get picked and run */

    /* Now wake normal_task — should preempt idle_task */
    kstep_task_wakeup(normal_task);
    kstep_tick();  /* Trigger reschedule */

    /* Check who is running on CPU 1 */
    struct rq *rq1 = cpu_rq(1);
    struct task_struct *curr = rq1->curr;

    if (curr == normal_task) {
        kstep_pass("normal_task preempted idle_task as expected");
    } else if (curr == idle_task) {
        kstep_fail("idle_task still running despite non-idle wakeup "
                   "(slice protection not cancelled)");
    }
}
```

### Additional diagnostic logging:

The driver should also log the `vlag` and `deadline` fields of the idle task's sched_entity before and after the wakeup to confirm whether slice protection was active and whether it was cancelled:

```c
printk("Before wakeup: idle_task se.vlag=%lld se.deadline=%lld (protected=%d)\n",
       idle_task->se.vlag, idle_task->se.deadline,
       idle_task->se.vlag == idle_task->se.deadline);
```

After waking `normal_task`, on a fixed kernel, `idle_task->se.vlag` should equal `idle_task->se.deadline + 1` (protection cancelled), while on a buggy kernel it should still equal `idle_task->se.deadline`.

### kSTEP configuration:

- **CPUs**: At least 2 (CPU 0 for driver, CPU 1 for test tasks).
- **No cgroup or topology changes needed**.
- **No kSTEP framework modifications needed**: `SCHED_IDLE` can be set via `sched_setattr_nocheck()` which is accessible from kernel module context. All other required APIs (`kstep_task_create`, `kstep_task_pin`, `kstep_task_wakeup`, `kstep_tick`, `cpu_rq()`) are already available.

### Expected behavior:

- **Buggy kernel (pre-fix)**: After waking `normal_task`, the idle task retains slice protection. `pick_eevdf()` returns the idle task via the RUN_TO_PARITY shortcut. The normal task must wait until the idle task's slice expires or it becomes ineligible (up to 3ms of latency).
- **Fixed kernel (post-fix)**: `check_preempt_wakeup_fair()` calls `cancel_protect_slice(se)` on the idle entity before `goto preempt`. When `pick_eevdf()` runs, `curr->vlag != curr->deadline`, so the RUN_TO_PARITY shortcut is skipped. The EEVDF algorithm runs normally and picks the non-idle entity with an earlier deadline. The normal task runs immediately.
