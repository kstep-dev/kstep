# RT: Permanent RT Task Throttling After Runtime Change From Unlimited

**Commit:** `9b58e976b3b391c0cf02e038d53dd0478ed3013c`
**Affected files:** kernel/sched/rt.c
**Fixed in:** v5.17-rc1
**Buggy since:** v2.6.26-rc1 (introduced by `d0b27fa77854` "sched: rt-group: synchonised bandwidth period")

## Bug Description

When the RT bandwidth runtime is changed from unlimited (`-1`) to a finite value while an RT task is already running, the task becomes permanently throttled and can never be unthrottled. This happens because the RT period timer, which is responsible for periodically replenishing RT bandwidth and unthrottling tasks, was never started during the unlimited-runtime phase and is not restarted when the runtime limit is subsequently imposed.

The exact reproduction sequence is:
1. Set `sched_rt_runtime_us` to `-1` (unlimited RT runtime) via `/proc/sys/kernel/sched_rt_runtime_us`.
2. Start a `SCHED_FIFO` task that runs continuously (e.g., a `while(1)` loop).
3. Change `sched_rt_runtime_us` back to a finite value (e.g., `950000`).

After step 3, the running RT task accumulates `rt_time` (execution time tracked against the RT bandwidth quota). Since the RT period timer was never activated, no periodic replenishment occurs. Once `rt_time` exceeds the newly imposed `rt_runtime`, the task is throttled via `sched_rt_runtime_exceeded()`. Because the period timer remains inactive (`rt_period_active == 0`), the `sched_rt_period_timer` callback never fires, `do_sched_rt_period_timer()` never resets `rt_time` or clears `rt_throttled`, and the task remains permanently throttled.

This is a liveness bug: any RT FIFO or RT RR task that was running when the runtime limit was imposed will be starved indefinitely. The only recovery is to set `sched_rt_runtime_us` back to `-1` and restart the task, or to reboot.

## Root Cause

The root cause lies in the interaction between `start_rt_bandwidth()`, the `sched_rt_period_timer`, and the `update_curr_rt()` function.

**The `start_rt_bandwidth()` guard:** When an RT task is enqueued (in `inc_rt_group()`), `start_rt_bandwidth()` is called to ensure the RT period timer is running. However, `start_rt_bandwidth()` has an early-return guard:

```c
static void start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
    if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
        return;
    // ... start the period timer ...
}
```

When `sched_rt_runtime_us == -1`, `rt_b->rt_runtime == RUNTIME_INF`, so the function returns immediately without activating the period timer. This is correct behavior at that point—there is no need for a period timer when runtime is unlimited.

**The sysctl write path:** When `sched_rt_runtime_us` is written via `/proc/sys/kernel/sched_rt_runtime_us`, the call chain is:
1. `sched_rt_handler()` — validates and applies the new value
2. `sched_rt_global_constraints()` — updates per-CPU `rt_rq->rt_runtime` values
3. `sched_rt_do_global()` — updates `def_rt_bandwidth.rt_runtime` and `def_rt_bandwidth.rt_period`

Critically, **none of these functions start the period timer**. The `sched_rt_do_global()` function (pre-fix) simply assigns the new runtime and period values to `def_rt_bandwidth` without any locking or timer activation:

```c
static void sched_rt_do_global(void)
{
    def_rt_bandwidth.rt_runtime = global_rt_runtime();
    def_rt_bandwidth.rt_period = ns_to_ktime(global_rt_period());
}
```

**The throttling path in `update_curr_rt()`:** On each scheduler tick (or task accounting update), `update_curr_rt()` accumulates `delta_exec` into `rt_rq->rt_time` and checks if the runtime is exceeded:

```c
if (sched_rt_runtime(rt_rq) != RUNTIME_INF) {
    raw_spin_lock(&rt_rq->rt_runtime_lock);
    rt_rq->rt_time += delta_exec;
    if (sched_rt_runtime_exceeded(rt_rq))
        resched_curr(rq);
    raw_spin_unlock(&rt_rq->rt_runtime_lock);
}
```

Once the runtime limit changes from `RUNTIME_INF` to a finite value, this code path becomes active. The already-accumulated `rt_time` (from running with unlimited runtime) immediately exceeds the new limit, `sched_rt_runtime_exceeded()` sets `rt_rq->rt_throttled = 1`, and the task is dequeued. But since `rt_period_active` is still `0`, no timer is running to call `do_sched_rt_period_timer()` which would replenish the runtime and unthrottle the task.

**The secondary issue (race in `sched_rt_do_global`):** The pre-fix `sched_rt_do_global()` updates `def_rt_bandwidth.rt_runtime` and `def_rt_bandwidth.rt_period` without holding `def_rt_bandwidth.rt_runtime_lock`. This creates a data race with any concurrent reader of these fields (e.g., `do_sched_rt_period_timer()` or `balance_runtime()`), potentially causing torn reads of the 64-bit `rt_runtime` or `rt_period` values on 32-bit systems.

## Consequence

The observable impact is **permanent starvation of RT tasks**. Once the bug is triggered:

1. The affected RT FIFO/RR task is descheduled and never gets to run again. The kernel prints `"sched: RT throttling activated"` once, but the throttling never resolves.
2. On a system where the affected task is the only high-priority RT task on its CPU, that CPU effectively loses its RT workload. CFS tasks will run instead.
3. If RT group scheduling (`CONFIG_RT_GROUP_SCHED`) is enabled, the throttling affects the entire RT run queue of the group, potentially starving all RT tasks in that group on the affected CPU.
4. The condition persists until the system is rebooted, the task is killed by other means (e.g., `kill -9`), or the administrator sets `sched_rt_runtime_us` back to `-1`. Even after setting it back to `-1`, the task may remain throttled until manually woken up, because the period timer still won't fire to clear the throttled state.

This is a significant operational issue for systems that dynamically adjust RT bandwidth limits, such as container orchestration systems that modify cgroup RT parameters, or administration scripts that temporarily remove RT limits for configuration and then reinstate them.

## Fix Summary

The fix makes three changes to `kernel/sched/rt.c`:

**1. Refactor `start_rt_bandwidth()` into two functions:**
The core timer-starting logic is extracted into `do_start_rt_bandwidth()`, which unconditionally checks `rt_period_active` and starts the period timer if needed. The original `start_rt_bandwidth()` becomes a thin wrapper that performs the `rt_bandwidth_enabled()` and `RUNTIME_INF` guards before delegating to `do_start_rt_bandwidth()`. This allows the timer-starting logic to be called directly from contexts where the `RUNTIME_INF` guard would be inappropriate.

```c
static inline void do_start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
    raw_spin_lock(&rt_b->rt_runtime_lock);
    if (!rt_b->rt_period_active) {
        rt_b->rt_period_active = 1;
        hrtimer_forward_now(&rt_b->rt_period_timer, ns_to_ktime(0));
        hrtimer_start_expires(&rt_b->rt_period_timer,
                              HRTIMER_MODE_ABS_PINNED_HARD);
    }
    raw_spin_unlock(&rt_b->rt_runtime_lock);
}

static void start_rt_bandwidth(struct rt_bandwidth *rt_b)
{
    if (!rt_bandwidth_enabled() || rt_b->rt_runtime == RUNTIME_INF)
        return;
    do_start_rt_bandwidth(rt_b);
}
```

**2. Start the period timer when runtime is exceeded in `update_curr_rt()`:**
After detecting that the RT runtime is exceeded (and after releasing `rt_rq->rt_runtime_lock`), the fix calls `do_start_rt_bandwidth(sched_rt_bandwidth(rt_rq))` to ensure the period timer is active. This is the critical fix: when a task gets throttled, the timer is guaranteed to be running so that `do_sched_rt_period_timer()` will eventually fire, replenish the runtime, and unthrottle the task.

```c
exceeded = sched_rt_runtime_exceeded(rt_rq);
if (exceeded)
    resched_curr(rq);
raw_spin_unlock(&rt_rq->rt_runtime_lock);
if (exceeded)
    do_start_rt_bandwidth(sched_rt_bandwidth(rt_rq));
```

Note that `do_start_rt_bandwidth()` is called outside `rt_rq->rt_runtime_lock` but acquires `rt_b->rt_runtime_lock` internally. The `exceeded` variable is saved before releasing the lock to avoid checking the condition after the lock is dropped.

**3. Add proper locking to `sched_rt_do_global()`:**
The fix wraps the updates to `def_rt_bandwidth.rt_runtime` and `def_rt_bandwidth.rt_period` in `raw_spin_lock_irqsave(&def_rt_bandwidth.rt_runtime_lock, flags)` to prevent data races with concurrent readers.

## Triggering Conditions

The bug requires the following precise conditions:

- **Kernel configuration:** `CONFIG_RT_GROUP_SCHED` is not strictly required. The non-group-sched path also calls `start_rt_bandwidth(&def_rt_bandwidth)` from `inc_rt_group()`, which has the same `RUNTIME_INF` early-return issue. Both group and non-group configurations are affected.
- **Initial state:** `sched_rt_runtime_us` must be set to `-1` (which sets `rt_b->rt_runtime = RUNTIME_INF`). This causes the period timer to not be started when RT tasks are enqueued.
- **Running RT task:** At least one `SCHED_FIFO` or `SCHED_RR` task must be running and accumulating `rt_time` while the runtime is unlimited. The task needs to have accumulated enough `rt_time` to exceed whatever finite runtime limit is subsequently imposed.
- **Runtime change:** `sched_rt_runtime_us` must be changed from `-1` to a finite positive value (e.g., 950000, which is the default). This changes `rt_b->rt_runtime` from `RUNTIME_INF` to a finite number, enabling the throttling check in `update_curr_rt()`.
- **Number of CPUs:** Any number of CPUs will work. The bug is per-CPU: each CPU's `rt_rq` has its own `rt_time` counter and throttled state.
- **Timing:** The RT task must have been running long enough for `rt_rq->rt_time` to exceed the new runtime limit. Since `rt_time` accumulates continuously when runtime is unlimited (there is no cap or reset), even a brief period of RT execution can trigger the bug if the new limit is low enough. In practice, any task that runs for more than one period (default: 1 second) will have accumulated enough time.
- **Reliability:** The bug is 100% deterministic once the above conditions are met. There is no race condition or timing dependency—the bug is a structural logic error where the timer is simply never started.

## Reproduce Strategy (kSTEP)

This bug can be reproduced deterministically using kSTEP with the following strategy:

**1. Task setup:**
Create one RT FIFO task pinned to CPU 1 (avoiding CPU 0 which is reserved for the driver). The task should be a simple busy-spinning task.

```c
struct task_struct *rt_task = kstep_task_create();
kstep_task_pin(rt_task, 1, 1);
kstep_task_fifo(rt_task);
```

**2. Set unlimited RT runtime:**
Before waking the task, use `kstep_sysctl_write` to disable RT bandwidth throttling:

```c
kstep_sysctl_write("kernel/sched_rt_runtime_us", "%d", -1);
```

This sets `def_rt_bandwidth.rt_runtime = RUNTIME_INF` and ensures that `start_rt_bandwidth()` will skip starting the period timer when the task is enqueued.

**3. Wake the RT task and let it run:**
Wake the task and advance several ticks to let it accumulate `rt_time`:

```c
kstep_task_wakeup(rt_task);
kstep_tick_repeat(20);  // Let the task run for ~20 ticks
```

During these ticks, `update_curr_rt()` is called but the `sched_rt_runtime(rt_rq) != RUNTIME_INF` check is false, so `rt_time` is not actually tracked (or rather, the entire block is skipped). Wait—let me reconsider. Actually looking at the code more carefully:

In `update_curr_rt()`, the loop checks `sched_rt_runtime(rt_rq) != RUNTIME_INF`. When `rt_runtime == RUNTIME_INF`, this condition is false and `rt_time` is NOT accumulated. But `rt_time` starts at 0 from `init_rt_rq()`. After changing the runtime to a finite value, `rt_time` will start accumulating from 0 on the next `update_curr_rt()` call.

So the scenario is more nuanced: the task won't immediately be throttled just because it ran for a while with unlimited runtime. Instead, after the runtime change, `rt_time` will accumulate normally, and the task will be throttled after accumulating more than `rt_runtime` nanoseconds within a single period. The bug is that once throttled, the task is NEVER unthrottled because the period timer is not running.

**Revised approach:**

After changing `sched_rt_runtime_us` to a finite value, we need to let the task run long enough for `rt_time` to exceed `rt_runtime`. The default period is 1,000,000 us = 1 second. If we set `rt_runtime` to 950000 us = 950 ms, the task needs to accumulate 950 ms of `rt_time`. With kSTEP's default tick interval, we need enough ticks to cover this.

Alternatively, set a very small `rt_runtime` to trigger throttling quickly:

```c
// Step 1: Set unlimited runtime
kstep_sysctl_write("kernel/sched_rt_runtime_us", "%d", -1);

// Step 2: Create and wake RT FIFO task
struct task_struct *rt_task = kstep_task_create();
kstep_task_pin(rt_task, 1, 1);
kstep_task_fifo(rt_task);
kstep_task_wakeup(rt_task);

// Step 3: Advance some ticks while unlimited (rt_time won't accumulate)
kstep_tick_repeat(5);

// Step 4: Change to very restrictive runtime (e.g., 1000 us = 1 ms per 1s period)
kstep_sysctl_write("kernel/sched_rt_runtime_us", "%d", 1000);

// Step 5: Advance enough ticks for rt_time to exceed 1 ms
kstep_tick_repeat(50);
```

**4. Observe the bug:**
After step 5, use `KSYM_IMPORT` to access the `cpu_rq(1)->rt` run queue and check:
- `rt_rq->rt_throttled` should be `1` (task is throttled).
- `def_rt_bandwidth.rt_period_active` should be `0` (timer not running) — this is the bug.

```c
struct rq *rq1 = cpu_rq(1);
struct rt_rq *rt_rq = &rq1->rt;
```

**5. Continue ticking to confirm permanent throttle:**
Advance many more ticks (e.g., 200+ ticks to cover multiple full periods). On a buggy kernel, `rt_rq->rt_throttled` should remain `1` because the period timer never fires. On a fixed kernel, the period timer fires, replenishes `rt_time`, unthrottles the task, and `rt_rq->rt_throttled` returns to `0`.

```c
kstep_tick_repeat(200);

// Check if task is still throttled
if (rt_rq->rt_throttled) {
    kstep_fail("RT task permanently throttled - period timer never fired");
} else {
    kstep_pass("RT task correctly unthrottled after period timer fired");
}
```

**6. Pass/fail criteria:**
- **Buggy kernel:** After many ticks (covering multiple RT periods), `rt_rq->rt_throttled == 1` and `def_rt_bandwidth.rt_period_active == 0`. The RT task is permanently descheduled. `kstep_output_curr_task()` on CPU 1 shows idle or a CFS task.
- **Fixed kernel:** After the first period timer fires, `rt_rq->rt_throttled == 0` (at least transiently) and the RT task runs again. `def_rt_bandwidth.rt_period_active == 1`. `kstep_output_curr_task()` on CPU 1 shows the RT task.

**7. Alternative observation method:**
Use `kstep_output_curr_task()` in an `on_tick_begin` callback to log which task is running on CPU 1 at each tick. On the buggy kernel, the RT task disappears and never returns. On the fixed kernel, it returns after the period timer replenishes the runtime.

**8. QEMU configuration:**
Configure QEMU with at least 2 CPUs. No special topology or memory configuration is needed.

**9. Kernel version guard:**
Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0) && LINUX_VERSION_CODE < KERNEL_VERSION(5,17,0)` (or similar) to match the window where the bug exists and kSTEP is supported. The bug was technically introduced in v2.6.26, but kSTEP only supports v5.15+.

**10. Important notes:**
- The `kstep_sysctl_write` function can write to `/proc/sys/kernel/sched_rt_runtime_us` to change the runtime dynamically.
- Access to `def_rt_bandwidth` requires `KSYM_IMPORT(def_rt_bandwidth)` since it is a static/non-exported symbol in `kernel/sched/rt.c`. Alternatively, access it via `cpu_rq(1)->rd->rt_period` or through the `sched_rt_bandwidth(rt_rq)` accessor using `kmod/internal.h`.
- The tick interval should be set such that ticks map to meaningful time deltas (at least a few milliseconds each) so that `rt_time` accumulates meaningfully against the `rt_runtime` threshold.
