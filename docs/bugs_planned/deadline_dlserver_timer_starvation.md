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

The fundamental challenge in reproducing this bug is that the triggering condition — the dl_server's deadline lagging far behind `rq_clock` — normally requires sustained heavy IRQ load that slows down `clock_task` relative to the wall clock. The previous strategy relied on directly writing to `dl_se->deadline` and `dl_se->runtime` to simulate this lag, but that violates the constraint against writing to internal scheduler fields. Instead, we exploit a timing-based approach using the kernel's **public debugfs interface** for dl_server configuration: we reconfigure the fair_server's CBS parameters to use the minimum allowed period (100 microseconds), while the kSTEP tick interval advances the clock by a much larger amount (10 milliseconds). This guarantees that by the time the first scheduler tick fires after the dl_server starts with the new parameters, the dl_server's deadline (only 100us into the future) has already been far surpassed by the clock advance (10ms), and the tiny runtime budget (50us) is also fully exhausted in the same tick's `delta_exec`. This deterministically triggers the exact code path where `start_dl_timer()` fails because `dl_next_period()` returns a time that is already in the past — the same failure mode as the original IRQ-induced bug on MediaTek platforms.

### QEMU Configuration

Use at least 2 CPUs: CPU 0 is reserved for the kSTEP driver, CPU 1 hosts the test scenario (the CFS task, the RT task, and the fair_server dl_server). Enable `CONFIG_DEBUG_FS` in the kernel build so that the fair_server debugfs interface is available at `/sys/kernel/debug/sched/fair_server/cpu1/`. The kernel source (in `kernel/sched/debug.c`) creates per-CPU debugfs directories under `/sys/kernel/debug/sched/fair_server/cpu<N>/` with `runtime` and `period` files that accept nanosecond values and go through `sched_server_write_common()`, which properly acquires the rq lock, calls `dl_server_stop()`, applies new parameters via `dl_server_apply_params()`, and calls `dl_server_start()`. No special boot parameters are needed beyond the standard kSTEP setup. Set `tick_interval_ns = 10000000` (10ms) in the driver configuration to guarantee a 100x gap between the dl_server's CBS period (100us) and the clock advance per tick.

### Step-by-step plan:

1. **Create workload tasks in `setup()`**:
   - Create one CFS task: `cfs_task = kstep_task_create()`. This task's presence on the runqueue activates the dl_server.
   - Create one SCHED_FIFO task: `rt_task = kstep_task_create()`. This is the RT starvation victim.

2. **Activate the dl_server on CPU 1 with default parameters**:
   Pin the CFS task to CPU 1 using `kstep_task_pin(cfs_task, 1, 1)` and wake it with `kstep_task_wakeup(cfs_task)`. Run `kstep_tick_repeat(5)` to let the dl_server initialize and start servicing the CFS task with the default CBS parameters (`dl_runtime=50ms`, `dl_period=1s`). At this point, the dl_server is actively running on CPU 1's DL runqueue, and the CFS task executes through it.

3. **Reconfigure the fair_server with a very short CBS period via debugfs**:
   Use `kstep_write()` to write to the debugfs interface. Write runtime first, then period, to avoid the `runtime > period` validation check in `sched_server_write_common()`:
   ```c
   // Set runtime to 50us (50,000 ns). Current period is 1s, so 50us < 1s passes validation.
   kstep_write("/sys/kernel/debug/sched/fair_server/cpu1/runtime", "50000\n", 6);
   // Set period to 100us (100,000 ns, the minimum: dl_server_period_min = 100 * NSEC_PER_USEC).
   // 50us runtime < 100us period passes validation.
   kstep_write("/sys/kernel/debug/sched/fair_server/cpu1/period", "100000\n", 7);
   ```
   Each debugfs write internally calls `dl_server_stop()` → `dl_server_apply_params()` → `dl_server_start()` under the rq lock. After the period write, the dl_server restarts with `dl_runtime=50,000ns`, `dl_period=100,000ns`, `dl_deadline=100,000ns`, and `deadline = rq_clock + 100,000ns` (100us into the future from the current mocked sched_clock). This is a **public kernel configuration interface**, not a direct write to internal scheduler fields.

4. **Introduce the RT (FIFO) task on CPU 1**:
   Convert the second task to SCHED_FIFO with `kstep_task_fifo(rt_task)`, pin it to CPU 1 with `kstep_task_pin(rt_task, 1, 1)`, and wake it with `kstep_task_wakeup(rt_task)`. The RT task enters CPU 1's RT runqueue but cannot run yet because the dl_server (DL scheduling class, which has strictly higher priority than RT) is active and serving the CFS task.

5. **Trigger the bug with a single tick**:
   Call `kstep_tick()`. This advances the mocked `sched_clock` by `tick_interval_ns` (10ms) and invokes `sched_tick()` on CPU 1 via `smp_call_function_single`. Inside `scheduler_tick()` → `task_tick_fair()` → `update_curr()` → `dl_server_update()` → `update_curr_dl_se()`, the following chain executes:
   - `update_rq_clock(rq)` sets `rq_clock = old_clock + 10ms`.
   - `delta_exec = rq_clock_task(rq) - dl_se->exec_start ≈ 10ms`.
   - `dl_se->runtime = 50us − 10ms = −9,950us` (deeply negative → `dl_runtime_exceeded()` returns true).
   - **Throttle path entered**: `dl_se->dl_throttled = 1`, then `dequeue_dl_entity(dl_se, 0)`.
   - `start_dl_timer(dl_se)` computes `act = dl_next_period(dl_se)`. Since `dl_next_period = deadline − dl_deadline + dl_period = deadline` (because `dl_deadline == dl_period == 100us`), and `deadline = old_clock + 100us` while `rq_clock = old_clock + 10ms`, the activation time is 9.9ms in the past. The `ktime_us_delta(act, now) < 0` check fires → `start_dl_timer()` returns 0 (failure).
   - **Buggy kernel**: `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)` is called → `replenish_dl_entity()` tries to advance the deadline in a loop, finds it still behind `rq_clock`, calls `replenish_dl_new_period()` to reset, clears `dl_throttled = 0`, and `__enqueue_dl_entity()` puts the dl_server back on the DL runqueue. The RT task remains starved.
   - **Fixed kernel**: `replenish_dl_new_period(dl_se, rq)` resets `deadline = rq_clock + dl_deadline` (now in the future), then `start_dl_timer(dl_se)` is called again — this time it succeeds because the activation time is in the future. `dl_throttled` **stays 1** (never cleared). The dl_server remains dequeued. The RT FIFO task gets scheduled.

6. **Observe the dl_server state (READ ONLY)**:
   After the critical tick returns, read CPU 1's fair_server fields to detect the bug:
   ```c
   struct rq *rq1 = cpu_rq(1);
   struct sched_dl_entity *dl_se = &rq1->fair_server;

   // Primary indicator: is the dl_server properly throttled?
   if (dl_se->dl_throttled == 0) {
       kstep_fail("dl_server not throttled after runtime exceeded — RT starvation");
   }

   // Secondary indicator: is the replenishment timer properly armed?
   if (dl_se->dl_throttled == 1 && hrtimer_active(&dl_se->dl_timer)) {
       kstep_pass("dl_server properly throttled with active replenishment timer");
   }
   ```
   All field reads (`dl_throttled`, `hrtimer_active()`) are purely observational. No internal state is modified.

7. **Verify RT task scheduling over multiple ticks (READ ONLY)**:
   Run `kstep_tick_repeat(20)` and monitor `rq1->curr->policy` in the `on_tick_end` callback:
   ```c
   struct rq *rq1 = cpu_rq(1);
   struct task_struct *curr = rq1->curr;
   if (curr->policy == SCHED_FIFO)
       kstep_pass("RT task running on CPU 1: pid=%d", curr->pid);
   else
       kstep_fail("RT task starved: curr pid=%d policy=%d", curr->pid, curr->policy);
   ```
   - **Buggy kernel**: `rq1->curr` remains the CFS task on every tick (the dl_server is perpetually re-enqueued without a throttle interval, creating an infinite cycle of runtime-exceeded → timer-fail → re-enqueue).
   - **Fixed kernel**: `rq1->curr` switches to the RT FIFO task during the dl_server's throttle interval. When the replenishment timer fires on a subsequent tick, the dl_server is re-activated and the CFS task runs briefly, then the cycle repeats normally.

### Why this works without writing to internal scheduler fields

The entire approach uses only public interfaces and read-only internal access:
- **debugfs** (`/sys/kernel/debug/sched/fair_server/cpu1/{runtime,period}`) is the kernel's official interface for reconfiguring dl_server CBS parameters, exposed to userspace and implemented in `kernel/sched/debug.c`. The write handler properly manages locking, stopping, parameter application, and restarting. This is fundamentally different from directly assigning to `dl_se->deadline` or `dl_se->runtime`.
- **kstep_write()** is a standard kSTEP API (`driver.h`) for writing to filesystem paths, using `filp_open` + `kernel_write` + `filp_close`.
- **Task creation, pinning, wakeup, and policy change** (`kstep_task_create`, `kstep_task_pin`, `kstep_task_wakeup`, `kstep_task_fifo`) are standard kSTEP APIs that invoke public kernel scheduling interfaces (`sched_setscheduler`, `set_cpus_allowed_ptr`, `wake_up_process`).
- **Tick progression** (`kstep_tick`, `kstep_tick_repeat`) is a kSTEP API that invokes `sched_tick()` on remote CPUs.
- **All reads** of `cpu_rq(1)->fair_server.dl_throttled`, `rq->curr`, `hrtimer_active()`, etc. are purely observational.

The key insight is that by making the CBS period (100us) much shorter than the tick interval (10ms), we deterministically create the condition where the deadline expires before the tick fires. This is functionally equivalent to the IRQ-induced time lag in the original MediaTek bug report: in both cases, the dl_server's runtime takes longer than one CBS period to be consumed (due to IRQ-slowed `clock_task` in the original case, or due to the tick granularity being coarser than the period in our case), causing the deadline to fall behind `rq_clock` by the time `update_curr_dl_se()` triggers the throttle path.

### kSTEP Driver Configuration:

```c
KSTEP_DRIVER_DEFINE{
    .name = "dlserver_timer_starvation",
    .setup = setup,
    .run = run,
    .on_tick_end = on_tick_end,      // observe rq->curr and dl_server state each tick
    .step_interval_us = 1000,        // 1ms real-time sleep between steps
    .tick_interval_ns = 10000000,    // 10ms virtual clock advance per tick (>> 100us period)
};
```

The `tick_interval_ns = 10ms` ensures the dl_server's 100us deadline is 100x behind the clock at the first tick, making the `start_dl_timer()` failure absolute and deterministic regardless of the kernel's HZ configuration. The `on_tick_end` callback runs after `sched_tick()` has executed and `kstep_sleep()` has given time for context switches, providing a reliable observation point.

### kSTEP Callbacks:
- Use `on_tick_end` to inspect `cpu_rq(1)->fair_server.dl_throttled`, `cpu_rq(1)->curr->policy`, and `hrtimer_active(&cpu_rq(1)->fair_server.dl_timer)` after each tick.
- Log the dl_server's `deadline`, `runtime`, `dl_throttled`, and the current task's `comm`/`pid`/`policy` on CPU 1 for diagnostic output.

### Expected behavior:
- **Buggy kernel (v6.8 – v6.17-rc3)**: After the critical tick, the dl_server is immediately re-enqueued with a fresh budget via `enqueue_dl_entity(ENQUEUE_REPLENISH)`. `dl_throttled == 0`. The `dl_timer` is not armed (`hrtimer_active` returns false). The CFS task continues running through the dl_server indefinitely because on every subsequent tick, the same cycle repeats: runtime exceeded → `start_dl_timer` fails (the short period guarantees the deadline is always behind) → re-enqueue with `ENQUEUE_REPLENISH`. The RT FIFO task is permanently starved. Test reports **FAIL**.
- **Fixed kernel (v6.17-rc4+)**: After the critical tick, `replenish_dl_new_period()` resets the deadline to `rq_clock + dl_deadline` (now in the future), then `start_dl_timer()` succeeds. `dl_throttled == 1`. The `dl_timer` is armed. The dl_server stays dequeued from the DL runqueue, and the RT FIFO task is scheduled to run on CPU 1 during the throttle interval. When the timer fires on a subsequent tick, the dl_server is replenished and serves the CFS task again briefly. Normal DL/RT cycling is observed. Test reports **PASS**.

### Potential kSTEP extensions needed:
- None fundamental. The driver reads `rq->fair_server` through kSTEP's internal access to `kernel/sched/sched.h` (via `cpu_rq()` macro). `hrtimer_active()` is a public inline function from `<linux/hrtimer.h>`. The `kstep_write()` API handles writing to debugfs paths.
- Ensure `CONFIG_DEBUG_FS=y` in the kernel build configuration (this is the default on most kernel configs and is required for the debugfs-based fair_server parameter interface).

### Guard with version check:
```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)
// dl_server / fair_server available since v6.8 (63ba8422f876)
// debugfs interface for fair_server parameters available in kernel/sched/debug.c
#endif
```
