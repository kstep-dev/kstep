# Deadline: DL Server Timer Failure Causes RT Task Starvation

**Commit:** `421fc59cf58c64f898cafbbbbda0bc705837e7df`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.17-rc4
**Buggy since:** v6.8-rc1 (introduced by `63ba8422f876` "sched/deadline: Introduce deadline servers")

## Bug Description

The Linux kernel's "fair server" mechanism uses a SCHED_DEADLINE entity (the `dl_server`) on each CPU's runqueue to ensure that CFS (fair) tasks receive a guaranteed share of CPU time even when higher-priority RT (SCHED_FIFO/SCHED_RR) tasks are present. The dl_server is configured with a deadline period (default 1 second) and runtime budget (default 50ms), functioning as a Constant Bandwidth Server (CBS) that periodically grants execution time to fair tasks.

When the dl_server exhausts its runtime budget, it should be throttled (dequeued from the DL runqueue) and a replenishment timer (`dl_timer`) should be armed to fire at the start of the next period. During the throttled interval, RT tasks have exclusive access to the CPU. However, a bug in the throttle path of `update_curr_dl_se()` causes the dl_server to be immediately re-enqueued when `start_dl_timer()` fails, effectively preventing RT tasks from ever running.

The failure of `start_dl_timer()` occurs when the dl_server's deadline has fallen so far behind the real clock that the computed timer expiration time is already in the past. This situation arises when the dl_server runs for a very long time without being throttled — for example, when heavy IRQ or IPI load on the CPU slows down the rate at which the dl_server's runtime is consumed (`delta_exec` is computed from `clock_task`, which excludes IRQ time). In such cases, the dl_server's CBS period elapses multiple times before its runtime budget is fully consumed, causing the deadline to lag far behind `rq_clock`.

When this happens, the buggy code path calls `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)`, which invokes `replenish_dl_entity()`. That function tries to advance the deadline by adding `dl_period` in a loop until the runtime is positive, but if the deadline is still behind `rq_clock` after this loop, it prints "sched: DL replenish lagged too much" and calls `replenish_dl_new_period()` to reset the deadline from the current clock. Crucially, `replenish_dl_entity()` then clears `dl_throttled = 0` and the entity is enqueued via `__enqueue_dl_entity()`. This puts the dl_server back on the runqueue immediately, allowing it to run again without any throttle period, starving RT tasks.

## Root Cause

The root cause lies in the `throttle:` label path within `update_curr_dl_se()` in `kernel/sched/deadline.c`. When the dl_server's runtime is exceeded (`dl_runtime_exceeded(dl_se)` returns true), the code flow is:

1. `dl_se->dl_throttled = 1` — mark the entity as throttled
2. `dequeue_dl_entity(dl_se, 0)` — remove it from the DL runqueue
3. `start_dl_timer(dl_se)` — attempt to arm the replenishment timer

The `start_dl_timer()` function computes the next activation time (`act`) based on `dl_next_period(dl_se)` (which is `deadline - dl_deadline + dl_period`). It then adjusts this time from `rq_clock` domain to `hrtimer` domain and checks: `if (ktime_us_delta(act, now) < 0) return 0;` — if the activation time is already in the past, it returns 0 (failure).

When the dl_server has been running for multiple periods without throttling (due to slow runtime consumption caused by heavy IRQ time), `dl_next_period(dl_se)` returns a value that, when adjusted, is still in the past relative to `hrtimer`'s current time. This causes `start_dl_timer()` to fail.

The buggy fallback code then executes:
```c
if (unlikely(is_dl_boosted(dl_se) || !start_dl_timer(dl_se))) {
    if (dl_server(dl_se))
        enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH);
    ...
}
```

The call to `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)` triggers `replenish_dl_entity()`, which:
- Tries to advance the deadline in a `while` loop: `dl_se->deadline += dl_period; dl_se->runtime += dl_runtime;`
- When the deadline is still behind `rq_clock`, calls `replenish_dl_new_period()` to reset from the current clock
- Clears `dl_se->dl_throttled = 0`
- Then `__enqueue_dl_entity()` places the dl_server back on the DL runqueue

The critical problem is that the dl_server is immediately re-enqueued with a fresh budget and no throttle period. On the very next scheduler tick, `update_curr_dl_se()` will again consume runtime, and if the same condition persists (e.g., continued heavy IRQ load), the timer will fail again, and the cycle repeats. The dl_server effectively runs continuously, never yielding to RT tasks.

The memory dump from the original bug report confirms this state: `dl_throttled = 0`, `dl_defer_running = 1`, `dl_server_active = 1`, with the timer's `state = 0` (not armed) and `expires` far in the past relative to `clock`.

## Consequence

The primary consequence is **starvation of SCHED_FIFO and SCHED_RR (real-time) tasks**. The dl_server, which exists to protect CFS tasks from RT starvation, itself becomes the cause of RT starvation when this bug is triggered. RT tasks on the affected runqueue cannot execute because the dl_server continuously occupies the DL scheduling class, which has higher priority than RT.

This is a severe issue for real-time systems where RT task latencies are critical. The bug was observed on MediaTek platforms (ARM64 SoCs) where IRQ load from hardware peripherals created the conditions for this failure. The symptom manifests as a complete scheduling inversion: fair tasks get CPU time through the dl_server while RT tasks are indefinitely delayed.

The warning message "sched: DL replenish lagged too much" serves as a diagnostic indicator that this condition has occurred, though the warning itself is rate-limited (`printk_deferred_once`). In production systems, this bug can cause missed real-time deadlines, system responsiveness degradation, and potential watchdog timeouts if critical RT tasks (like watchdog kthreads) are starved.

## Fix Summary

The fix modifies the fallback handling in the `throttle:` path of `update_curr_dl_se()` when `start_dl_timer()` fails for a `dl_server` entity. Instead of calling `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)` which clears the throttled state and re-enqueues the entity, the fix calls:

```c
if (dl_server(dl_se)) {
    replenish_dl_new_period(dl_se, rq);
    start_dl_timer(dl_se);
} else {
    enqueue_task_dl(rq, dl_task_of(dl_se), ENQUEUE_REPLENISH);
}
```

The key changes are:
1. **`replenish_dl_new_period(dl_se, rq)`** resets the dl_server's deadline to `rq_clock + dl_deadline` and runtime to `dl_runtime`, starting a completely fresh CBS period from the current time. This ensures the next timer expiration will be in the future.
2. **`start_dl_timer(dl_se)`** is called again after the fresh replenishment. Since `replenish_dl_new_period()` sets a new deadline relative to the current `rq_clock`, the computed activation time will now be in the future, so `start_dl_timer()` will succeed.
3. Critically, the dl_server's `dl_throttled` flag **remains set to 1** (it was set earlier in the throttle path and is not cleared). The entity stays dequeued from the DL runqueue until the timer fires, giving RT tasks their rightful execution window.

This approach ensures that even when the dl_server has fallen far behind the wall clock, it properly throttles itself and arms a valid timer for future replenishment, rather than immediately resuming execution and starving RT tasks.

## Triggering Conditions

The bug requires the following specific conditions:

1. **A dl_server must be active on a CPU**: This is the default configuration since v6.8 when deadline servers were introduced. The fair server runs with `dl_runtime = 50ms` and `dl_period = 1000ms` by default.

2. **The dl_server must be actively servicing CFS tasks**: There must be runnable CFS tasks on the CPU being served by the dl_server, with `dl_defer_running = 1` (the server is actively running in non-deferred mode, servicing starving CFS tasks).

3. **The dl_server's runtime consumption must be extremely slow**: The runtime accounting uses `clock_task` which excludes time spent in hardirq context. If the CPU experiences heavy IRQ load (many hardware interrupts, frequent IPIs), the dl_server's `runtime` decreases very slowly relative to wall clock time. This causes the CBS period to elapse (deadline passes) before the runtime budget is exhausted.

4. **The deadline must fall far enough behind rq_clock**: When `dl_runtime_exceeded()` finally returns true (runtime <= 0), the dl_server's deadline must be so far in the past that `dl_next_period()` (which computes `deadline - dl_deadline + dl_period`) still returns a past time. This requires the lag to be at least one full period behind.

5. **RT tasks must be runnable on the same CPU**: For the starvation to be observable, there must be SCHED_FIFO or SCHED_RR tasks waiting to run on the same CPU where the dl_server is misbehaving.

6. **At least 2 CPUs**: CPU 0 is reserved for the kSTEP driver, so the dl_server and RT tasks should be on CPU 1 or higher.

The triggering probability depends on the magnitude and duration of IRQ load. In the original MediaTek report, the condition was triggered by sustained heavy IRQ/IPI activity on a mobile SoC. In a controlled environment, the condition can be simulated by artificially advancing the `rq_clock` ahead of the dl_server's deadline, or by manipulating the dl_server's deadline/runtime to simulate the lagged state.

## Reproduce Strategy (kSTEP)

The core challenge is reproducing the condition where `start_dl_timer()` fails because the dl_server's deadline has fallen behind `rq_clock`. In a real system this happens due to heavy IRQ load slowing down runtime consumption, but in kSTEP we can trigger the same code path by directly manipulating the dl_server's internal state.

### Step-by-step plan:

1. **QEMU Configuration**: Use at least 2 CPUs (CPU 0 for driver, CPU 1 for the test scenario).

2. **Import required symbols**: Use `KSYM_IMPORT` to access internal scheduler structures:
   ```c
   KSYM_IMPORT(cpu_rq);  // or use cpu_rq() macro from sched.h internals
   ```
   Access the per-CPU runqueue's `fair_server` (`rq->fair_server`), which is the `struct sched_dl_entity` for the dl_server.

3. **Create workload tasks**:
   - Create one CFS task pinned to CPU 1 and wake it up. This ensures the dl_server will be active (there's fair work to service).
   - Create one SCHED_FIFO task pinned to CPU 1 using `kstep_task_fifo(p)`. This is the RT task that should be running but will be starved.

4. **Let the system stabilize**: Run `kstep_tick_repeat(10)` to allow the dl_server to start servicing the CFS task and establish normal operation.

5. **Manipulate the dl_server's deadline to simulate the lagged condition**:
   Access the `fair_server` on CPU 1's runqueue:
   ```c
   struct rq *rq1 = cpu_rq(1);
   struct sched_dl_entity *dl_se = &rq1->fair_server;
   ```
   Set the dl_server's deadline far in the past relative to `rq_clock(rq1)`:
   ```c
   // Simulate the condition where multiple CBS periods have elapsed
   // Set deadline to be several periods behind rq_clock
   dl_se->deadline = rq_clock(rq1) - 2 * dl_se->dl_period;
   // Set runtime to a small positive value so it exhausts quickly
   dl_se->runtime = 1000;  // 1 microsecond of remaining runtime
   ```

6. **Trigger the throttle path**: Run `kstep_tick()` to cause `update_curr_dl_se()` to be called. The small remaining runtime will be consumed, triggering the `throttle:` path. With the deadline far in the past, `start_dl_timer()` will fail, and the buggy code path will be taken.

7. **Observe the bug**: After the tick, check the dl_server's state:
   ```c
   // On buggy kernel: dl_throttled should be 0 (improperly unthrottled)
   // and the entity should be enqueued on the DL runqueue
   if (dl_se->dl_throttled == 0 && on_dl_rq(dl_se)) {
       kstep_fail("dl_server re-enqueued without throttle period");
   }
   ```

8. **Check RT task scheduling**: Run several more ticks and observe which task runs on CPU 1:
   ```c
   kstep_tick_repeat(20);
   // On buggy kernel: the CFS task keeps running via dl_server
   // On fixed kernel: the RT (FIFO) task should be running
   struct task_struct *curr = rq1->curr;
   if (curr->policy == SCHED_FIFO) {
       kstep_pass("RT task is running after dl_server throttle");
   } else {
       kstep_fail("RT task starved: curr policy=%d", curr->policy);
   }
   ```

9. **Alternative detection via timer state**: Check whether `dl_timer` is properly armed:
   ```c
   // On buggy kernel: timer state is 0 (not armed), entity not throttled
   // On fixed kernel: timer state is active, entity throttled
   if (dl_se->dl_throttled == 1 && hrtimer_active(&dl_se->dl_timer)) {
       kstep_pass("dl_server properly throttled with active timer");
   } else {
       kstep_fail("dl_server not properly throttled: throttled=%d timer_active=%d",
                  dl_se->dl_throttled, hrtimer_active(&dl_se->dl_timer));
   }
   ```

### Expected behavior:
- **Buggy kernel**: After manipulating the deadline and triggering the throttle path, the dl_server will be immediately re-enqueued (`dl_throttled = 0`, on the DL runqueue). The RT task will not get to run. The `on_tick_begin` callback will show the CFS task (via dl_server) continuing to run.
- **Fixed kernel**: The dl_server will remain throttled (`dl_throttled = 1`) with a properly armed timer. The RT (FIFO) task will be scheduled to run on CPU 1 during the throttle interval.

### kSTEP Callbacks:
- Use `on_tick_begin` or `on_tick_end` to inspect `rq->fair_server.dl_throttled`, `rq->curr->policy`, and `hrtimer_active(&rq->fair_server.dl_timer)` on each tick.
- Log the dl_server's `deadline`, `runtime`, `dl_throttled`, and the current task's comm/pid/policy on CPU 1.

### Potential kSTEP extensions needed:
- None fundamental. The driver can directly access `rq->fair_server` through `internal.h` / `sched.h` includes, and manipulate the `deadline` and `runtime` fields. The `cpu_rq()` macro and `rq_clock()` are already available through kSTEP's internal access to `kernel/sched/sched.h`.
- If `hrtimer_active()` is not directly accessible, the driver can check `dl_se->dl_timer.state` directly.

### Guard with version check:
```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)
// dl_server / fair_server available since v6.8
#endif
```
