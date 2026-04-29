# Deadline: dl_server Getting Stuck Due to Premature server_has_tasks Check

**Commit:** `4ae8d9aa9f9dc7137ea5e564d79c5aa5af1bc45c`
**Affected files:** `kernel/sched/deadline.c`, `kernel/sched/fair.c`, `kernel/sched/sched.h`, `include/linux/sched.h`
**Fixed in:** v6.17
**Buggy since:** v6.17-rc1 (commit `cccb45d7c4295` "sched/deadline: Less agressive dl_server handling")

## Bug Description

The Linux kernel's SCHED_DEADLINE subsystem provides a "dl_server" mechanism — a deadline-scheduled server entity (`rq->fair_server`) that provides bandwidth guarantees to CFS (fair) tasks. The dl_server is configured with a runtime/period budget (default: 50ms/1000ms) and ensures that CFS tasks receive at least their allocated share of CPU time, even in the presence of RT (SCHED_FIFO/SCHED_RR) tasks that would otherwise starve them.

Commit `cccb45d7c4295` ("sched/deadline: Less agressive dl_server handling") introduced a lazy stopping mechanism to reduce overhead. Instead of immediately calling `dl_server_stop()` and `dl_server_start()` every time CFS tasks arrive or depart, this commit added a `dl_server_idle` flag and a two-period grace mechanism via `dl_server_stopped()`: the first period without fair tasks sets `dl_server_idle = 1`, and only if a second consecutive period also finds no tasks does the server actually stop. The `dl_server_update()` function clears `dl_server_idle` whenever the server actually executes fair work, resetting this two-strike counter.

However, this commit also introduced a critical bug: the `dl_server_timer()` callback and `replenish_dl_entity()` check `dl_se->server_has_tasks()` at bandwidth replenishment time to decide whether to arm the zero-laxity defer timer. When `server_has_tasks()` returns false at that exact moment (because there are momentarily no CFS tasks queued), the timer path takes an early exit — calling `replenish_dl_entity()` followed by `dl_server_stopped()` and then returning `HRTIMER_NORESTART`. This leaves the dl_server in a state where it is dequeued from the DL runqueue, has `dl_server_active` still set (because `dl_server_stopped()` only sets `dl_server_idle = 1` on the first call), and has no timer pending. Subsequent calls to `dl_server_start()` see `dl_server_active` is already set and return immediately without re-enqueueing the server. The dl_server is now permanently dead.

## Root Cause

The root cause is the `server_has_tasks()` check at two critical decision points in `dl_server_timer()` and `replenish_dl_entity()`. In the buggy code:

**In `dl_server_timer()`** (lines ~1174-1177 of the buggy `deadline.c`):
```c
if (!dl_se->server_has_tasks(dl_se)) {
    replenish_dl_entity(dl_se);
    dl_server_stopped(dl_se);
    return HRTIMER_NORESTART;
}
```
When the bandwidth timer fires and finds no CFS tasks at that instant, it replenishes the dl_server's runtime, calls `dl_server_stopped()` (which on first call just sets `dl_server_idle = 1` and returns false), and exits without restarting the timer. The dl_server remains throttled/dequeued with no timer to re-enqueue it.

**In `replenish_dl_entity()`** (line ~878 of the buggy `deadline.c`):
```c
if (!is_dl_boosted(dl_se) && dl_se->server_has_tasks(dl_se)) {
    dl_se->dl_defer_armed = 1;
    dl_se->dl_throttled = 1;
    if (!start_dl_timer(dl_se)) { ... }
}
```
When replenishment occurs but `server_has_tasks()` returns false, the defer timer is NOT armed. This means even if `replenish_dl_entity()` is called (e.g., from `dl_server_timer()`), the deferred server mechanism that would normally schedule the zero-laxity timer to re-enqueue the server is skipped.

The resulting stuck state is:
1. `dl_se->dl_server_active = 1` (set previously when the server was started)
2. `dl_se->dl_server_idle = 1` (set by `dl_server_stopped()`)
3. The dl_server is NOT on the DL runqueue (it was dequeued or never re-enqueued)
4. No `dl_timer` is pending (the timer returned `HRTIMER_NORESTART`)
5. `dl_se->dl_throttled = 0` (cleared by `replenish_dl_entity()`)

When a CFS task later arrives and `enqueue_task_fair()` calls `dl_server_start()`:
```c
void dl_server_start(struct sched_dl_entity *dl_se)
{
    if (!dl_server(dl_se) || dl_se->dl_server_active)
        return;  // <-- early exit because dl_server_active is still 1!
    ...
}
```
The function sees `dl_server_active == 1` and returns immediately. The dl_server is never re-enqueued. From this point on, RT tasks can permanently starve CFS tasks because the dl_server bandwidth mechanism is completely dead.

The fundamental issue is that checking whether there are currently CFS tasks at the moment of bandwidth refresh is semantically wrong. As the commit message states: "it is totally irrelevant if there are fair tasks at the moment of bandwidth refresh." The correct behavior is for the bandwidth timer to always start the zero-laxity timer, which in turn will enqueue the dl_server and cause `server_pick_task()` to be called — and the pick function will return NULL if there are no fair tasks, which triggers the existing `dl_server_stopped()` two-period idle detection logic through `__pick_task_dl()`.

## Consequence

The bug causes **complete CFS task starvation** on the affected CPU. Once the dl_server gets stuck, RT tasks (SCHED_FIFO, SCHED_RR) on that CPU can monopolize the CPU indefinitely without the dl_server intervening to provide CFS tasks their minimum bandwidth guarantee.

This was discovered by John Stultz who hit lockup warnings when running `locktorture` (a kernel lock testing module) on a 2-CPU VM. The locktorture workload creates RT tasks that contend heavily for locks, and without the dl_server providing bandwidth to CFS tasks, critical kernel threads and other CFS-scheduled work on that CPU are completely starved. This leads to RCU stall warnings, soft lockup detection, and eventually system hang — the classic symptoms of indefinite CFS starvation by RT tasks.

The bug is particularly dangerous because it is silent: there is no warning or error message when the dl_server enters the stuck state. The system simply becomes progressively less responsive on the affected CPU as CFS tasks wait indefinitely for their turn that never comes. On a 2-CPU system, losing fair scheduling on one CPU can effectively hang the entire system since many essential kernel threads are CFS-scheduled.

## Fix Summary

The fix removes the `server_has_tasks()` callback entirely from the kernel. Three specific changes eliminate the bug:

1. **In `dl_server_timer()`**: The `if (!dl_se->server_has_tasks(dl_se))` early-exit branch is completely removed. The timer now always proceeds to arm the defer timer or enqueue the dl_server directly, regardless of whether there are currently CFS tasks. This ensures the dl_server lifecycle is governed by the pick/yield mechanism in `__pick_task_dl()` rather than by a racy point-in-time check in the timer path.

2. **In `replenish_dl_entity()`**: The condition `&& dl_se->server_has_tasks(dl_se)` is removed from the defer timer arming logic. The new code is simply `if (!is_dl_boosted(dl_se))`. This ensures the zero-laxity defer timer is always armed when appropriate, letting the dl_server be properly scheduled and allowing `server_pick_task()` to discover and handle the no-tasks case correctly.

3. **Structural cleanup**: The `server_has_tasks` function pointer is removed from `struct sched_dl_entity`, the `fair_server_has_tasks()` function is removed from `fair.c`, the `dl_server_has_tasks_f` typedef is removed, and `dl_server_init()` no longer takes a `has_tasks` parameter. This eliminates all users of the callback, making it impossible for similar bugs to be re-introduced.

The fix is correct because the proper mechanism for handling the "no CFS tasks" case already exists: when `__pick_task_dl()` picks the dl_server and calls `server_pick_task()`, if it returns NULL, the code calls `dl_server_stopped()` which implements the two-period idle detection. After two consecutive periods without fair tasks, the server is properly stopped. This mechanism does not suffer from the race condition because it operates during the actual scheduling decision, not at an arbitrary timer callback moment.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **Kernel version**: The bug exists only in kernels containing commit `cccb45d7c4295` ("sched/deadline: Less agressive dl_server handling") but not containing the fix `4ae8d9aa9f9d`. This corresponds to the v6.17-rc1 through v6.17-rc6 range (approximately).

2. **dl_server enabled**: The fair server must be active (default configuration; `dl_runtime > 0`). The default is 50ms runtime with 1000ms period.

3. **RT tasks present**: At least one SCHED_FIFO or SCHED_RR task must be running on the target CPU. The RT task(s) must be able to starve CFS tasks — meaning they are CPU-bound or at least running frequently enough to prevent CFS tasks from executing.

4. **CFS task absence at timer callback**: The critical timing requirement is that the dl_server's bandwidth timer (`dl_timer`) fires at a moment when there are zero CFS tasks queued on the runqueue (`rq->cfs.nr_queued == 0`). This can happen in several ways:
   - All CFS tasks on that CPU have blocked (sleeping, waiting for I/O, etc.)
   - CFS tasks have migrated away from the CPU
   - CFS bandwidth throttling has removed all CFS tasks from the runqueue
   - The workload naturally has bursty CFS activity with gaps

5. **Subsequent CFS task arrival**: After the dl_server enters the stuck state, at least one CFS task must arrive (via wakeup or migration) on the same CPU, triggering `dl_server_start()` which will fail due to the stuck `dl_server_active` flag.

6. **CPU count**: A 2-CPU configuration makes this particularly easy to trigger, as demonstrated by the reporter. With RT tasks on one CPU and occasional CFS task absence, the probability of hitting the window is high. More CPUs dilute the probability but do not eliminate it.

The bug is relatively easy to trigger with workloads that combine RT tasks with intermittent CFS tasks on the same CPU. The `locktorture` kernel module is an effective trigger because it creates RT-priority threads that contend for locks, creating patterns of CFS task blocking and waking that frequently hit the vulnerable timing window.

## Reproduce Strategy (kSTEP)

The goal is to reproduce the state where the dl_server gets stuck: `dl_server_active == 1`, the server is NOT on the DL runqueue, and no timer is pending. Here is a step-by-step plan for a kSTEP driver:

### Setup

1. **QEMU configuration**: 2 CPUs (minimum). CPU 0 is reserved for the driver, so the bug is triggered on CPU 1.
2. **No special topology needed** — default flat topology is sufficient.
3. **Kernel config**: Default scheduler config with `CONFIG_SMP=y`. No special sysctl writes needed; the dl_server uses its default 50ms/1000ms budget.

### Task Creation and Pinning

1. Create one SCHED_FIFO (RT) task pinned to CPU 1 using `kstep_task_create()`, `kstep_task_fifo(rt_task)`, and `kstep_task_pin(rt_task, 1, 2)`.
2. Create one CFS task pinned to CPU 1 using `kstep_task_create()` and `kstep_task_pin(cfs_task, 1, 2)`.

### Reproducing the Stuck State

The core approach is to create a scenario where the dl_server's bandwidth timer fires when there are no CFS tasks on CPU 1's runqueue.

**Step 1**: Wake up the RT task on CPU 1 (`kstep_task_wakeup(rt_task)`). This makes the RT task runnable and running on CPU 1 (since it has highest priority).

**Step 2**: Wake up the CFS task on CPU 1 (`kstep_task_wakeup(cfs_task)`). This calls `enqueue_task_fair()` which calls `dl_server_start()`, starting the dl_server. The dl_server is now active and enqueued on the DL runqueue. With the deferred server mechanism, the dl_server will be throttled with a defer timer armed.

**Step 3**: Advance time using `kstep_tick_repeat()` to allow the dl_server to begin consuming its period. The dl_server should eventually get throttled (its runtime exhausted) or the defer timer should fire.

**Step 4**: Block the CFS task (`kstep_task_block(cfs_task)`) so that `rq->cfs.nr_queued` drops to 0 on CPU 1. The critical requirement is that this happens BEFORE the dl_server's bandwidth timer fires for replenishment.

**Step 5**: Continue advancing ticks (`kstep_tick_repeat()`) until the dl_server's `dl_timer` fires for bandwidth replenishment. Since `server_has_tasks()` returns false (no CFS tasks queued), the buggy code path in `dl_server_timer()` will:
   - Call `replenish_dl_entity()` (which won't arm the defer timer because `server_has_tasks()` is false)
   - Call `dl_server_stopped()` (which sets `dl_server_idle = 1` but does NOT stop the server since it's the first idle period)
   - Return `HRTIMER_NORESTART`

At this point the dl_server is in the stuck state.

**Step 6**: Wake up the CFS task again (`kstep_task_wakeup(cfs_task)`). This calls `enqueue_task_fair()` → `dl_server_start()`, which checks `dl_se->dl_server_active` and finds it is 1, so it returns immediately without enqueueing the server.

### Detection / Observation

Use `KSYM_IMPORT` to access the internal state of `cpu_rq(1)->fair_server`:

```c
KSYM_IMPORT(cpu_rq);
```

After step 6, check the following fields on `cpu_rq(1)->fair_server`:
- `dl_server_active` should be `1` (stuck active)
- `dl_server_idle` should be `1`
- The dl_server should NOT be on the DL runqueue (check `sched_dl_runnable(rq)` or `dl_rq->dl_nr_running`)
- `dl_throttled` should be `0`
- The `dl_timer` should not be pending (`hrtimer_active(&dl_se->dl_timer)` should be 0)

Also observe that the CFS task is NOT getting scheduled despite being runnable. Use the `on_tick_begin` callback to monitor `rq->curr` on CPU 1 — it should always be the RT task, never the CFS task. On the buggy kernel, after the dl_server gets stuck, the CFS task will be starved indefinitely. Use `kstep_output_curr_task()` in the tick callback.

**Pass/fail criteria**:
- On the **buggy kernel**: After waking the CFS task in step 6, advance many ticks (e.g., `kstep_tick_repeat(2000)` to cover multiple dl_server periods). The CFS task should NEVER run. Check `cpu_rq(1)->fair_server.dl_server_active == 1` AND the fair_server is not on the DL runqueue. Call `kstep_fail("dl_server stuck: active=%d, on_rq=%d, timer_active=%d", ...)`.
- On the **fixed kernel**: After waking the CFS task, the dl_server will properly restart (since `dl_server_start()` now functions correctly — the `server_has_tasks` check in the timer path is gone, so the timer properly arms the defer timer, which re-enqueues the server). The CFS task should get scheduled within one dl_server period (1000ms = ~1000 ticks at 1ms resolution). Call `kstep_pass(...)`.

### Key Timing Considerations

The trickiest part is ensuring the dl_server's timer fires while there are no CFS tasks. Since kSTEP controls task blocking and tick progression precisely, this can be achieved deterministically:
1. After starting both tasks, tick enough times for the dl_server's bandwidth period to begin winding down.
2. Block the CFS task before the timer fires.
3. Continue ticking past the timer expiry.

To determine exact tick counts, read `fair_server.deadline` and `fair_server.runtime` after initial setup to calculate when the bandwidth timer will fire. Alternatively, use `kstep_tick_until()` with a callback that checks `fair_server.dl_throttled` to advance until the server becomes throttled, then block the CFS task and tick through the replenishment.

### Alternative Approach (Simpler)

A simpler approach that may also work: never wake the CFS task initially. Only wake the RT task. Let the dl_server be started by some transient CFS activity (kSTEP setup itself may trigger this). Then wait for the dl_server bandwidth timer to cycle through. Since there are no persistent CFS tasks, `server_has_tasks()` returns false at timer time, triggering the stuck state. Then wake a CFS task and observe it being starved. This avoids the need for precise timing of the CFS task block relative to the timer.

### kSTEP Extensions Needed

No extensions to the kSTEP framework are required. The existing APIs (`kstep_task_create`, `kstep_task_fifo`, `kstep_task_pin`, `kstep_task_wakeup`, `kstep_task_block`, `kstep_tick_repeat`, `KSYM_IMPORT`) and internal access to `cpu_rq()` and the `sched_dl_entity` fields are sufficient to reproduce this bug.
