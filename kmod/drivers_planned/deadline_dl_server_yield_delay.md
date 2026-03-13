# Deadline: dl_server yields instead of stopping, causing 1-second fair task delay

**Commit:** `a3a70caf7906708bf9bbc80018752a6b36543808`
**Affected files:** `kernel/sched/deadline.c`, `kernel/sched/sched.h`, `include/linux/sched.h`
**Fixed in:** v6.17
**Buggy since:** v6.17-rc1 (commit `cccb45d7c4295` "sched/deadline: Less agressive dl_server handling")

## Bug Description

The dl_server (deadline server) is a mechanism that provides SCHED_DEADLINE bandwidth to CFS (fair) tasks. It runs as a deferred deadline entity with a default period of 1000ms and runtime of 50ms per CPU. When RT (FIFO) tasks are starving fair tasks, the dl_server periodically intervenes to give CFS tasks CPU time. This is critical for preventing indefinite starvation of fair tasks by real-time workloads.

Commit `cccb45d7c4295` ("sched/deadline: Less agressive dl_server handling") introduced a two-phase idle mechanism (`dl_server_stopped()`) to reduce overhead from frequent `dl_server_start()`/`dl_server_stop()` transitions. Instead of immediately stopping the server when there are no fair tasks to run, it would first mark the server as "idle" (`dl_server_idle = 1`) and only actually stop it on the second consecutive invocation where no fair tasks are found. The intent was to avoid paying the start/stop overhead for workloads that rapidly transition between 0 and 1 CFS tasks.

However, when `dl_server_stopped()` returned false on its first call (server not yet idle), the code in `__pick_task_dl()` would set `dl_yielded = 1` and call `update_curr_dl_se(rq, dl_se, 0)`. This yielded the server's current runtime, which pushed the server's next scheduling opportunity out by an entire period — 1 second with default parameters. For fair tasks that briefly go idle (e.g., blocking on I/O for a few milliseconds) while FIFO tasks are spinning, this meant the fair workload would only get one scheduling invocation per second, which is catastrophically slow.

John Stultz reported this behavior: when deliberately starving fair tasks with spinning FIFO tasks, his fair workload — which often goes briefly idle — experienced fair invocations delayed by a full second, resulting in only one invocation per period. This was both unexpected and terribly slow compared to the pre-`cccb45d7c4295` behavior.

## Root Cause

The root cause lies in the `__pick_task_dl()` function in `kernel/sched/deadline.c` and the `dl_server_stopped()` helper it called. The buggy code path was:

```c
// __pick_task_dl() in the buggy kernel:
if (dl_server(dl_se)) {
    p = dl_se->server_pick_task(dl_se);
    if (!p) {
        if (!dl_server_stopped(dl_se)) {
            dl_se->dl_yielded = 1;
            update_curr_dl_se(rq, dl_se, 0);
        }
        goto again;
    }
    ...
}
```

The `dl_server_stopped()` function implemented a two-phase idle detection:

```c
static bool dl_server_stopped(struct sched_dl_entity *dl_se)
{
    if (!dl_se->dl_server_active)
        return true;

    if (dl_se->dl_server_idle) {
        dl_server_stop(dl_se);
        return true;
    }

    dl_se->dl_server_idle = 1;
    return false;
}
```

On the first call when `server_pick_task()` returns NULL and the server is active and not yet marked idle, `dl_server_stopped()` sets `dl_server_idle = 1` and returns `false`. This causes the caller to execute:

1. `dl_se->dl_yielded = 1` — marks the server as having yielded.
2. `update_curr_dl_se(rq, dl_se, 0)` — called with delta_exec=0, which processes the yield flag.

Inside `update_curr_dl_se()`, when `dl_yielded` is set, the function calls `dl_yield(dl_se)` which zeroes the remaining runtime (`dl_se->runtime = 0`). This triggers `dl_server_timer` to be started for the next replenishment, which is at the beginning of the next period — up to 1 second away. The server is now effectively deferred for an entire period.

The critical problem is the interaction between two behaviors:
- The `dl_server_idle` flag is cleared in `dl_server_update()` whenever the server accounts runtime (i.e., a fair task actually runs). So if a fair task runs even briefly, `dl_server_idle` resets to 0.
- When the fair task goes idle (blocks), `server_pick_task()` returns NULL, and the first-phase idle check fires again, yielding the server again.

This creates a pathological cycle: fair task runs → `dl_server_idle` cleared → fair task blocks → pick returns NULL → `dl_server_stopped()` returns false (first phase) → yield → wait 1 second → repeat. The fair workload gets exactly one invocation per period.

## Consequence

The observable impact is severe performance degradation for CFS workloads running alongside SCHED_FIFO tasks. Specifically:

When SCHED_FIFO tasks occupy a CPU and fair tasks on that CPU frequently go briefly idle (which is normal for interactive or I/O-bound workloads), the dl_server will yield its remaining runtime every time it finds no runnable fair task during the first pick attempt. This pushes the next fair scheduling opportunity out by the server's full period (default: 1 second). Instead of getting prompt service when a fair task wakes up, the task must wait for the server's next period to begin, resulting in approximately 1 invocation per second.

This effectively turns the dl_server's starvation protection from a 50ms-per-second bandwidth guarantee into a mechanism that provides only a single brief scheduling opportunity per second, and even that opportunity may be wasted if the fair task happens to be blocked at the exact moment the server fires. The result is that interactive or I/O-bound CFS tasks become almost entirely starved despite the dl_server being specifically designed to prevent this. John Stultz confirmed this behavior was "both unexpected and terribly slow" in his testing.

There is no crash or kernel panic — this is purely a performance and fairness issue. However, it fundamentally undermines the purpose of the dl_server mechanism, which was added (commit `557a6bfc662c`) specifically to guarantee minimum CPU bandwidth to fair tasks even under RT starvation.

## Fix Summary

The fix replaces the two-phase idle mechanism with a direct `dl_server_stop()` call. In `__pick_task_dl()`, when `server_pick_task()` returns NULL (no runnable fair tasks), the server is immediately stopped:

```c
// Fixed __pick_task_dl():
if (dl_server(dl_se)) {
    p = dl_se->server_pick_task(dl_se);
    if (!p) {
        dl_server_stop(dl_se);
        goto again;
    }
    ...
}
```

This removes the yield entirely. When the server stops, it retains its accumulated bandwidth. When a fair task subsequently wakes up, `enqueue_task_fair()` calls `dl_server_start()`, which restarts the server. Subject to CBS (Constant Bandwidth Server) wakeup rules, the restarted server can use its remaining bandwidth without waiting for a new period. This means a fair task waking up shortly after the server stopped can be scheduled almost immediately.

The fix also removes the `dl_server_stopped()` function, the `dl_server_idle` bitfield from `struct sched_dl_entity`, and the clearing of `dl_server_idle` in `dl_server_update()`. The commit message explains that this does not re-introduce the overhead problem that `cccb45d7c4295` tried to solve: any start/stop cycle is naturally throttled by the timer period (no active timer cancel in `dl_server_stop()` — wait, actually it does call `hrtimer_try_to_cancel()`, but the key insight is that the dl_defer behavior means the server timer only fires at the zero-laxity point, so repeated start/stop cycles within a single period don't cause excessive timer reprogramming). Additionally, the updated documentation in `sched.h` clarifies the full lifecycle: the server starts on task enqueue, runs until either runtime is exhausted or no tasks remain, and stops itself when `server_pick_task()` returns NULL.

## Triggering Conditions

The following conditions are needed to trigger this bug:

1. **Kernel version**: Must be running a kernel containing commit `cccb45d7c4295` but not the fix `a3a70caf7906`. This means kernels v6.17-rc1 through v6.17-rc6 (exclusive of the fix).

2. **Task mix**: At least one SCHED_FIFO task must be running (spinning) on a CPU, and at least one CFS task must be pinned to the same CPU. The FIFO task starves the CFS task, requiring the dl_server to provide bandwidth.

3. **Fair task behavior**: The CFS task must periodically go idle (block) even if briefly. This is the critical trigger — when the task blocks, `server_pick_task()` returns NULL, triggering the yield path. A CFS task that never blocks will not trigger the bug because the server will always find a runnable task.

4. **CPU configuration**: At least 2 CPUs (one for the kSTEP driver on CPU 0, one for the test workload). The dl_server operates per-CPU, so only the CPU where both the FIFO and CFS tasks are running matters.

5. **dl_server enabled**: The fair_server must be initialized with non-zero runtime (default: 50ms runtime, 1000ms period). This is the default configuration in modern kernels.

6. **Timing**: The bug is deterministic — it will occur every time the server picks and finds no runnable fair tasks after the fair task has run at least once (clearing `dl_server_idle`). No race condition or specific timing window is needed. The yield happens on the very first pick attempt after the fair task blocks.

7. **Observation window**: The effect becomes observable over multiple server periods (seconds). The fair task will get approximately one scheduling opportunity per second instead of prompt service.

## Reproduce Strategy (kSTEP)

The strategy is to demonstrate that the buggy kernel delays fair task execution by a full dl_server period (~1 second) after the fair task goes briefly idle, while the fixed kernel provides prompt service.

### Step-by-step plan:

1. **QEMU configuration**: 2 CPUs (CPU 0 for driver, CPU 1 for test). Default memory is fine.

2. **Task setup**:
   - Create one SCHED_FIFO task (`rt_task`) and pin it to CPU 1. This task will run continuously to create the starvation scenario.
   - Create one CFS task (`fair_task`) and pin it to CPU 1. This task will be periodically blocked and woken to simulate the brief idle pattern.

3. **Sequence of operations**:
   - Start the FIFO task on CPU 1: `kstep_task_fifo(rt_task)`, `kstep_task_pin(rt_task, 1, 2)`, `kstep_task_wakeup(rt_task)`.
   - Wake the CFS task on CPU 1: `kstep_task_pin(fair_task, 1, 2)`, `kstep_task_wakeup(fair_task)`.
   - Advance ticks to let the dl_server pick up the fair task and run it briefly. Use `kstep_tick_repeat()` with enough ticks to cover at least one dl_server invocation.
   - Block the fair task: `kstep_task_block(fair_task)`.
   - Advance a few ticks — the dl_server should attempt to pick a task, find none, and yield (buggy) or stop (fixed).
   - Wake the fair task: `kstep_task_wakeup(fair_task)`.
   - Advance ticks and observe how quickly the fair task gets scheduled. On the buggy kernel, it should take up to 1 second (the server period) for the dl_server to fire again. On the fixed kernel, `dl_server_start()` is called from `enqueue_task_fair()` and the fair task should be scheduled promptly.

4. **Detection method using internal state access**:
   - Access `cpu_rq(1)->fair_server` via kSTEP's `internal.h` (which includes `kernel/sched/sched.h`).
   - After blocking the fair task and advancing a few ticks, check `cpu_rq(1)->fair_server.dl_yielded`. On the buggy kernel, this will be 1. On the fixed kernel, the server will be stopped (`dl_server_active == 0`) and `dl_yielded` will be 0.
   - After waking the fair task, track how many ticks (time) elapse before the fair task becomes the current task on CPU 1. Use `on_tick_begin` callback to check `cpu_rq(1)->curr` each tick.
   - Use `KSYM_IMPORT` if needed to access any non-exported symbols.

5. **Callbacks**:
   - `on_tick_begin`: Check `cpu_rq(1)->curr` to see if the fair task is running. Log the tick count when the fair task first runs after being woken. Also log the state of `cpu_rq(1)->fair_server` (dl_server_active, dl_yielded, dl_throttled, runtime).

6. **Pass/fail criteria**:
   - After waking the fair task, count the number of ticks until the fair task runs on CPU 1.
   - **Buggy kernel**: The fair task should take a very long time (approaching 1 second / many hundreds of ticks) to run because the dl_server yielded and won't fire until the next period.
   - **Fixed kernel**: The fair task should run within a few ticks because `dl_server_start()` immediately enqueues the server, which can preempt the RT task.
   - Use `kstep_pass()` if the fair task runs within a reasonable number of ticks (< 100 ticks) and `kstep_fail()` if it takes more (indicating the yield delay).
   - Alternatively, directly check the fair_server state: on the buggy kernel, after the pick fails, `dl_yielded` will be 1 and `runtime` will be 0; on the fixed kernel, `dl_server_active` will be 0 (stopped).

7. **Expected behavior**:
   - **Buggy kernel**: After blocking and re-waking the fair task, the dl_server has yielded its runtime. The server's replenishment timer won't fire for up to 1 second (the period). During this time, the fair task cannot run because the FIFO task has higher priority and the dl_server is not providing bandwidth. `kstep_fail()` should fire.
   - **Fixed kernel**: After blocking the fair task, the dl_server stops. When the fair task wakes, `enqueue_task_fair()` calls `dl_server_start()`, which enqueues the dl_server entity. The server can preempt the FIFO task (if it has remaining runtime) and schedule the fair task promptly. `kstep_pass()` should fire.

8. **kSTEP tick configuration**: The default tick interval should work. The dl_server's period is 1000ms and runtime is 50ms. With the default tick interval of ~4ms (250 HZ), we need ~250 ticks per second. Run at least 300-500 ticks after the wake to observe the delay (or lack thereof).

9. **Potential kSTEP considerations**: The dl_server is initialized by `sched_init_dl_servers()` during boot. kSTEP's `kstep_reset_runqueues()` may affect the fair_server state. If the dl_server is not active after reset, the driver may need to enqueue a CFS task first to trigger `dl_server_start()` from `enqueue_task_fair()`. The driver should verify the dl_server is active before proceeding with the test.

10. **Alternative detection**: Instead of counting ticks, the driver can log timestamps. Record `rq_clock(cpu_rq(1))` when the fair task is woken and when it first runs. The difference should be small (< 50ms) on the fixed kernel and large (~1000ms) on the buggy kernel.
