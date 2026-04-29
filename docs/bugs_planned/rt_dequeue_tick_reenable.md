# RT: Missed Tick Re-enabling on RT Task Dequeue (nohz_full)

**Commit:** `5c66d1b9b30f737fcef85a0b75bfe0590e16b62a`
**Affected files:** `kernel/sched/rt.c`
**Fixed in:** v6.0-rc1
**Buggy since:** v4.6 (introduced by `76d92ac305f2` — "sched: Migrate sched to use new tick dependency mask model")

## Bug Description

When a SCHED_RT task is dequeued from a runqueue on a `nohz_full` CPU, the scheduler tick is supposed to be re-evaluated. If the dequeued task was the last RT task and only multiple SCHED_OTHER (CFS) tasks remain, the tick must be re-enabled so that CFS time-slicing can work correctly. However, due to an ordering bug in the RT scheduler's dequeue path, the tick dependency check sees stale state and incorrectly concludes that RT tasks are still present, preventing the tick from being re-enabled.

The root cause is that `dequeue_top_rt_rq()` calls `sub_nr_running()`, which triggers `sched_update_tick_dependency()` → `sched_can_stop_tick()`. At the time of this call, `rq->rt.rt_nr_running` has not yet been decremented — that happens later in `__dequeue_rt_entity()` → `dec_rt_tasks()`. So `sched_can_stop_tick()` observes a non-zero `rt_nr_running` and decides the tick can be stopped (since FIFO tasks don't need tick-based preemption), even though the RT task is being removed and only CFS tasks will remain, which do need the tick for involuntary preemption.

This bug was introduced in v4.6 when commit `76d92ac305f2` migrated the scheduler to use the new tick dependency mask model. That commit added calls to `sched_update_tick_dependency()` inside `sub_nr_running()` and `add_nr_running()`, expecting that class-specific counters (like `rt_nr_running`) would already be updated before these functions are called. Every other scheduler class (CFS, deadline) follows this convention, but the RT scheduler's `dequeue_rt_stack()` function called `dequeue_top_rt_rq()` (which calls `sub_nr_running()`) before calling `__dequeue_rt_entity()` (which calls `dec_rt_tasks()` to decrement `rt_nr_running`).

## Root Cause

The bug lies in the ordering of operations within `dequeue_rt_stack()` in `kernel/sched/rt.c`. Before the fix, the function looked like:

```c
static void dequeue_rt_stack(struct sched_rt_entity *rt_se, unsigned int flags)
{
    struct sched_rt_entity *back = NULL;
    for_each_sched_rt_entity(rt_se) {
        rt_se->back = back;
        back = rt_se;
    }
    dequeue_top_rt_rq(rt_rq_of_se(back));   // (1) calls sub_nr_running()
    for (rt_se = back; rt_se; rt_se = rt_se->back) {
        if (on_rt_rq(rt_se))
            __dequeue_rt_entity(rt_se, flags); // (2) calls dec_rt_tasks()
    }
}
```

At step (1), `dequeue_top_rt_rq()` calls `sub_nr_running(rq, rt_rq->rt_nr_running)`, which decrements `rq->nr_running` and then calls `sched_update_tick_dependency(rq)`. This function checks `tick_nohz_full_cpu(cpu)` and then calls `sched_can_stop_tick(rq)`.

Inside `sched_can_stop_tick()`, the logic checks RT task counts:
```c
bool sched_can_stop_tick(struct rq *rq)
{
    if (rq->dl.dl_nr_running)
        return false;
    if (rq->rt.rr_nr_running) {
        if (rq->rt.rr_nr_running == 1)
            return true;
        else
            return false;
    }
    fifo_nr_running = rq->rt.rt_nr_running - rq->rt.rr_nr_running;
    if (fifo_nr_running)
        return true;          // <-- BUG: this path is taken with stale rt_nr_running
    if (rq->cfs.h_nr_queued > 1)
        return false;
    return true;
}
```

Because `rt_nr_running` has not been decremented yet (step (2) hasn't executed), the function sees `fifo_nr_running > 0` for a SCHED_FIFO task being dequeued, and returns `true` (tick can stop). This causes `tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED)` to be called, clearing the scheduler's tick dependency. The tick is then not re-enabled even though the remaining CFS tasks need it.

At step (2), `__dequeue_rt_entity()` → `dec_rt_tasks()` finally decrements `rq->rt.rt_nr_running`, but by then the tick dependency decision has already been made incorrectly.

The `dequeue_top_rt_rq()` function itself used `rt_rq->rt_nr_running` as the count to subtract from `rq->nr_running`:
```c
static void dequeue_top_rt_rq(struct rt_rq *rt_rq)
{
    sub_nr_running(rq, rt_rq->rt_nr_running);
    rt_rq->rt_queued = 0;
}
```

This meant that when called before `dec_rt_tasks()`, it would subtract the full (not-yet-decremented) `rt_nr_running` from `rq->nr_running` — which is correct for the `nr_running` accounting itself, but the side-effect of triggering the tick dependency check with stale `rt_nr_running` is the bug.

## Consequence

On `nohz_full` systems, after the last RT (SCHED_FIFO) task on a CPU finishes or blocks, the scheduler tick on that CPU may fail to be re-enabled. If multiple SCHED_OTHER (CFS) tasks remain on the CPU, they require the periodic tick for involuntary preemption (time-slicing). Without the tick, CFS tasks can monopolize the CPU without being preempted, leading to:

1. **CFS task starvation**: One CFS task can run indefinitely without being preempted, starving all other CFS tasks on that CPU. This is particularly harmful for latency-sensitive workloads.

2. **Violation of nohz_full guarantees**: The `nohz_full` feature is designed to stop the tick only when it's safe (single task or tasks that don't need tick-based preemption). The bug causes the tick to remain stopped when it should be active, breaking the fundamental correctness guarantee of the tick dependency tracking system.

3. **Subtle timing-dependent failures**: Because the bug depends on the specific transition from having RT tasks to having only CFS tasks on a `nohz_full` CPU, it may manifest intermittently in real workloads. The affected CPU may appear to hang from the perspective of some tasks, while one task continues running. This is difficult to diagnose as there is no crash or warning — just incorrect scheduling behavior.

The bug was reported and discussed on LKML. Reviewer Valentin Schneider confirmed the analysis and the fix approach. The bug has existed since v4.6 (2016) through v5.19, affecting any `nohz_full` system that mixes RT and CFS tasks.

## Fix Summary

The fix inverts the order of operations in `dequeue_rt_stack()` so that `__dequeue_rt_entity()` (which calls `dec_rt_tasks()` to decrement `rt_nr_running`) executes before `dequeue_top_rt_rq()` (which calls `sub_nr_running()` and triggers the tick dependency check).

Because `dequeue_top_rt_rq()` previously used the live `rt_rq->rt_nr_running` value as the count to subtract from `rq->nr_running`, and that value would now be decremented before the call, the fix also changes `dequeue_top_rt_rq()` to accept an explicit `count` parameter. The count is captured before the dequeue loop runs:

```c
static void dequeue_rt_stack(struct sched_rt_entity *rt_se, unsigned int flags)
{
    struct sched_rt_entity *back = NULL;
    unsigned int rt_nr_running;
    for_each_sched_rt_entity(rt_se) {
        rt_se->back = back;
        back = rt_se;
    }
    rt_nr_running = rt_rq_of_se(back)->rt_nr_running; // snapshot count
    for (rt_se = back; rt_se; rt_se = rt_se->back) {
        if (on_rt_rq(rt_se))
            __dequeue_rt_entity(rt_se, flags); // decrements rt_nr_running first
    }
    dequeue_top_rt_rq(rt_rq_of_se(back), rt_nr_running); // now triggers tick check
}
```

The `dequeue_top_rt_rq()` signature changes from `dequeue_top_rt_rq(struct rt_rq *rt_rq)` to `dequeue_top_rt_rq(struct rt_rq *rt_rq, unsigned int count)`, and uses `count` instead of `rt_rq->rt_nr_running` in the call to `sub_nr_running()`. All callers (`sched_rt_rq_dequeue()` for both CONFIG_RT_GROUP_SCHED and !CONFIG_RT_GROUP_SCHED cases) are updated to pass `rt_rq->rt_nr_running` as the count parameter.

This fix is correct because: (1) the snapshot of `rt_nr_running` before the dequeue loop preserves the correct value for `rq->nr_running` accounting, and (2) by the time `sched_update_tick_dependency()` runs inside `sub_nr_running()`, `rq->rt.rt_nr_running` has already been decremented by `dec_rt_tasks()`, so `sched_can_stop_tick()` sees the correct, updated state. This matches the convention used by every other scheduler class.

## Triggering Conditions

The bug requires the following conditions:

- **CONFIG_NO_HZ_FULL=y**: The kernel must be compiled with full dynticks support. Without this, `sched_update_tick_dependency()` is a no-op and the bug has no effect.

- **nohz_full CPU**: The target CPU must be in the `nohz_full` set (configured via the `nohz_full=` kernel boot parameter). The check `tick_nohz_full_cpu(cpu)` must return true for the CPU where the RT task is being dequeued.

- **At least 1 SCHED_FIFO task on the nohz_full CPU**: A SCHED_FIFO task (not SCHED_RR) must be running or runnable on the target CPU. SCHED_FIFO is specifically needed because `sched_can_stop_tick()` returns `true` when `fifo_nr_running > 0` (FIFO tasks don't need tick for round-robin). If using SCHED_RR, the tick dependency is handled differently through `rr_nr_running`.

- **Multiple SCHED_OTHER (CFS) tasks on the same CPU**: After the RT task is dequeued, there must be 2 or more CFS tasks remaining on the CPU. If only 1 CFS task remains, the tick can legitimately be stopped (no need for time-slicing). The bug manifests when `rq->cfs.h_nr_queued > 1` would make `sched_can_stop_tick()` return `false`, but the stale `rt_nr_running` causes an early `return true`.

- **The RT task must be dequeued**: The FIFO task must be dequeued (e.g., by blocking, exiting, or migrating). The dequeue triggers `dequeue_task_rt()` → `dequeue_rt_entity()` → `dequeue_rt_stack()`, which is where the ordering bug exists.

- **At least 2 CPUs**: CPU 0 cannot be a `nohz_full` CPU (it's the housekeeping CPU). So a second CPU is needed for the nohz_full role.

The bug is 100% deterministic given these conditions — it is not a race condition. Every single dequeue of the last SCHED_FIFO task on a `nohz_full` CPU with multiple CFS tasks will trigger the incorrect tick dependency decision. The effect is that `TICK_DEP_BIT_SCHED` is cleared when it should be set, so the tick stops firing on that CPU and CFS tasks lose time-slicing.

## Reproduce Strategy (kSTEP)

### Overview

This bug can be reproduced in kSTEP by creating a scenario where a SCHED_FIFO task is dequeued from a `nohz_full` CPU that also has multiple CFS tasks. The key observable is the `TICK_DEP_BIT_SCHED` (bit 2) in the per-CPU `tick_sched.tick_dep_mask`. On the buggy kernel, this bit will be incorrectly cleared after the RT dequeue; on the fixed kernel, it will be correctly set.

### Prerequisites

The kernel must be built with `CONFIG_NO_HZ_FULL=y` and booted with the parameter `nohz_full=1` (marking CPU 1 as a nohz_full CPU). kSTEP will need a minor extension to support passing custom kernel boot parameters to QEMU (or the kSTEP kernel config needs `CONFIG_NO_HZ_FULL=y`). The QEMU VM must have at least 2 CPUs.

### Step-by-step Plan

1. **Topology setup**: Configure QEMU with at least 2 CPUs. CPU 0 is the housekeeping CPU (reserved for the driver). CPU 1 will be the nohz_full CPU.

2. **Create CFS tasks**: Create 3 SCHED_OTHER (CFS) tasks and pin them to CPU 1 using `kstep_task_pin(p, 1, 2)`. These tasks ensure that after the RT task dequeue, there are multiple CFS tasks on the runqueue, requiring the tick for time-slicing.

   ```c
   struct task_struct *cfs1 = kstep_task_create();
   struct task_struct *cfs2 = kstep_task_create();
   struct task_struct *cfs3 = kstep_task_create();
   kstep_task_pin(cfs1, 1, 2);
   kstep_task_pin(cfs2, 1, 2);
   kstep_task_pin(cfs3, 1, 2);
   ```

3. **Create SCHED_FIFO task**: Create a SCHED_FIFO task and pin it to CPU 1.

   ```c
   struct task_struct *rt_task = kstep_task_create();
   kstep_task_fifo(rt_task);
   kstep_task_pin(rt_task, 1, 2);
   ```

4. **Let tasks settle**: Run a few ticks to let all tasks get enqueued and the scheduler state stabilize.

   ```c
   kstep_tick_repeat(10);
   ```

5. **Import tick_sched accessor**: Use `KSYM_IMPORT` to access `tick_get_tick_sched` which returns the per-CPU `tick_sched` structure, or directly access the per-CPU `tick_cpu_sched` variable.

   ```c
   KSYM_IMPORT(tick_get_tick_sched);
   ```

6. **Verify initial state**: Before dequeuing the RT task, verify that `tick_nohz_full_cpu(1)` returns true and record the current state of `rq->rt.rt_nr_running` on CPU 1 (should be 1).

   ```c
   struct rq *rq1 = cpu_rq(1);
   kstep_pass("Before dequeue: rt_nr_running=%d, nr_running=%u",
              rq1->rt.rt_nr_running, rq1->nr_running);
   ```

7. **Block the RT task to trigger dequeue**: Call `kstep_task_block(rt_task)` to dequeue the SCHED_FIFO task from CPU 1. This triggers the `dequeue_task_rt()` → `dequeue_rt_entity()` → `dequeue_rt_stack()` path that contains the bug.

   ```c
   kstep_task_block(rt_task);
   ```

8. **Check tick dependency state**: After the block, read the `tick_dep_mask` from the per-CPU `tick_sched` structure for CPU 1. Check whether `TICK_DEP_BIT_SCHED` (bit 2) is set.

   ```c
   struct tick_sched *ts = tick_get_tick_sched(1);
   int dep_mask = atomic_read(&ts->tick_dep_mask);
   int sched_dep = dep_mask & (1 << 2); // TICK_DEP_BIT_SCHED = 2

   kstep_pass("After RT dequeue: tick_dep_mask=0x%x, SCHED bit=%d",
              dep_mask, !!sched_dep);
   ```

9. **Pass/Fail criteria**:
   - **Buggy kernel**: `TICK_DEP_BIT_SCHED` is **cleared** (0) after the RT task dequeue, even though 3 CFS tasks need the tick. `sched_can_stop_tick()` incorrectly returned `true` because it saw stale `rt_nr_running > 0`.
   - **Fixed kernel**: `TICK_DEP_BIT_SCHED` is **set** (1) after the RT task dequeue, because `sched_can_stop_tick()` correctly sees `rt_nr_running == 0` and `cfs.h_nr_queued > 1`, returning `false`.

   ```c
   if (sched_dep) {
       kstep_pass("TICK_DEP_BIT_SCHED correctly set after RT dequeue");
   } else {
       kstep_fail("TICK_DEP_BIT_SCHED incorrectly cleared - bug triggered!");
   }
   ```

10. **Additional verification**: Also check `rq->rt.rt_nr_running` is 0 and `rq->cfs.h_nr_queued >= 2` at the time of the check, confirming the expected runqueue state.

### kSTEP Extensions Needed

- **Boot parameter support**: kSTEP needs to pass `nohz_full=1` as a kernel boot parameter to the QEMU VM. This is a minor change to the QEMU launch script or `run.py` configuration.
- **CONFIG_NO_HZ_FULL**: The kSTEP kernel configuration must include `CONFIG_NO_HZ_FULL=y`. This is a Kconfig change in the kernel build.
- **tick_sched access**: The driver needs access to `tick_get_tick_sched()` or the per-CPU `tick_cpu_sched` to read `tick_dep_mask`. This can be achieved via `KSYM_IMPORT(tick_get_tick_sched)`.

### Alternative Observation Method

If accessing `tick_dep_mask` proves difficult, an alternative is to observe the scheduling behavior directly: after blocking the RT task, if the tick is not re-enabled, the CFS tasks will not be preempted. The driver can observe this by checking `rq->curr` on CPU 1 across multiple tick periods — if the same CFS task remains current without switching, the tick is not firing and the bug is confirmed. On the fixed kernel, CFS tasks should round-robin after the RT task is removed.

```c
kstep_task_block(rt_task);
struct task_struct *first_curr = cpu_rq(1)->curr;
kstep_tick_repeat(100);
struct task_struct *later_curr = cpu_rq(1)->curr;

if (first_curr == later_curr) {
    kstep_fail("CFS tasks not preempting - tick not re-enabled (bug)");
} else {
    kstep_pass("CFS tasks preempting normally - tick re-enabled (fixed)");
}
```

This behavioral test is a more robust end-to-end verification that does not depend on internal tick state accessors.
