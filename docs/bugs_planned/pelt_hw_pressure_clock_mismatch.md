# PELT: hw_pressure Clock Source Mismatch Between sched_tick and Blocked Load Update

**Commit:** `84d265281d6cea65353fc24146280e0d86ac50cb`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.12-rc1
**Buggy since:** v6.10-rc1 (introduced by commit `97450eb90965` "sched/pelt: Remove shift of thermal clock")

## Bug Description

The PELT (Per-Entity Load Tracking) subsystem in the Linux kernel scheduler maintains running averages for various signals including RT load, DL load, IRQ time, and hardware pressure (`hw_pressure`). Each of these signals uses a `sched_avg` structure whose `last_update_time` field records the last clock value used to compute the PELT decay. All callers of the PELT update functions must consistently use the same clock source for a given `sched_avg` instance; otherwise, the time delta computation (`now - sa->last_update_time`) produces incorrect values.

Commit `97450eb90965` ("sched/pelt: Remove shift of thermal clock") was intended to simplify the hw_pressure clock by removing the configurable `sched_hw_decay_shift` that previously allowed scaling the thermal clock period. Before this commit, hw_pressure used `rq_clock_hw()` which was defined as `rq_clock_task(rq) >> sched_hw_decay_shift`. The commit removed `rq_clock_hw()` entirely and replaced both call sites — but used **different** replacement clocks in each location. In `sched_tick()`, the call was changed to use `rq_clock_task(rq)`, but in `__update_blocked_others()`, the call was changed to pass the local variable `now` which is set to `rq_clock_pelt(rq)`.

This introduced a clock domain mismatch: the same `sched_avg` structure (`rq->avg_hw`) is now updated alternately using `rq_clock_task()` from `sched_tick()` and `rq_clock_pelt()` from `__update_blocked_others()`. These two clock sources diverge whenever the CPU is running at reduced capacity, because `rq_clock_pelt()` scales time by CPU capacity while `rq_clock_task()` does not. The result is that `last_update_time` oscillates between two clock domains, causing either negative time deltas (detected and handled by resetting `last_update_time`) or artificially inflated positive deltas, both of which corrupt the hw_pressure PELT signal.

## Root Cause

The root cause lies in the `__update_blocked_others()` function in `kernel/sched/fair.c`. In the buggy version, the function begins by computing `u64 now = rq_clock_pelt(rq)` and passes this `now` value to all four PELT update functions:

```c
static bool __update_blocked_others(struct rq *rq, bool *done)
{
    const struct sched_class *curr_class;
    u64 now = rq_clock_pelt(rq);       /* PELT-invariant clock */
    unsigned long hw_pressure;
    bool decayed;

    curr_class = rq->curr->sched_class;
    hw_pressure = arch_scale_hw_pressure(cpu_of(rq));

    decayed = update_rt_rq_load_avg(now, rq, curr_class == &rt_sched_class) |
              update_dl_rq_load_avg(now, rq, curr_class == &dl_sched_class) |
              update_hw_load_avg(now, rq, hw_pressure) |   /* BUG: uses rq_clock_pelt */
              update_irq_load_avg(rq, 0);
    ...
}
```

Meanwhile, in `sched_tick()` in `kernel/sched/core.c`:

```c
void sched_tick(void)
{
    ...
    update_rq_clock(rq);
    hw_pressure = arch_scale_hw_pressure(cpu_of(rq));
    update_hw_load_avg(rq_clock_task(rq), rq, hw_pressure);  /* uses rq_clock_task */
    ...
}
```

The critical difference between the two clocks is:
- `rq_clock_task(rq)` returns `rq->clock_task`, which is the raw wall-clock time minus IRQ time.
- `rq_clock_pelt(rq)` returns `rq->clock_pelt - rq->lost_idle_time`, where `clock_pelt` is scaled by `arch_scale_cpu_capacity()` during busy periods. This means it advances more slowly when the CPU runs at reduced capacity.

The `update_rq_clock_pelt()` function, called from `update_rq_clock()`, scales each time delta:

```c
static inline void update_rq_clock_pelt(struct rq *rq, s64 delta)
{
    if (unlikely(is_idle_task(rq->curr))) {
        _update_idle_rq_clock_pelt(rq);  /* sync to clock_task when idle */
        return;
    }
    delta = cap_scale(delta, arch_scale_cpu_capacity(cpu_of(rq)));
    ...
    rq->clock_pelt += delta;
}
```

When a CPU runs at reduced capacity (e.g., half), each nanosecond of wall time advances `clock_pelt` by only ~0.5 ns, while `clock_task` advances by the full nanosecond. After many ticks on a busy CPU, a significant gap accumulates between the two clocks.

Inside `___update_load_sum()`, the delta is computed as:

```c
delta = now - sa->last_update_time;
if ((s64)delta < 0) {
    sa->last_update_time = now;  /* clock went backwards, reset */
    return 0;
}
delta >>= 10;
if (!delta) return 0;
sa->last_update_time += delta << 10;
```

Consider the following interleaving on a CPU at half capacity:
1. **sched_tick()** calls `update_hw_load_avg(rq_clock_task(rq)=1000, ...)` → `last_update_time` set to ~1000 (in rq_clock_task domain).
2. **__update_blocked_others()** calls `update_hw_load_avg(rq_clock_pelt(rq)=500, ...)` → `delta = 500 - 1000` = negative → detected by `(s64)delta < 0`, so `last_update_time` is reset to 500. No PELT update occurs.
3. **Next sched_tick()** calls `update_hw_load_avg(rq_clock_task(rq)=1004, ...)` → `delta = 1004 - 500 = 504` → this delta is artificially inflated (should have been ~4 ns), causing excessive decay.

This pattern repeats on every tick cycle, systematically corrupting the hw_pressure PELT signal.

## Consequence

The immediate consequence is corruption of the `rq->avg_hw` PELT signal (hw_pressure load average). The hw_pressure signal is used by the Energy Aware Scheduler (EAS) and other scheduling heuristics to account for reduced CPU capacity due to hardware thermal throttling. When this signal is corrupted:

1. **Excessive PELT decay**: The inflated time deltas cause the `load_sum`, `runnable_sum`, and `util_sum` in `rq->avg_hw` to decay far more rapidly than they should. This makes the kernel underestimate the hardware pressure on the CPU. Scheduling decisions based on `hw_load_avg()` would incorrectly assume the CPU has more available capacity than it actually does, leading to task placement on thermally throttled CPUs.

2. **Missed PELT updates**: When the negative delta case is hit, `___update_load_sum()` returns 0 without performing any accumulation. This means legitimate hw_pressure updates from `__update_blocked_others()` are silently dropped. Over time, this creates gaps in the PELT signal that make it unresponsive to actual hardware pressure changes.

3. **Incorrect frequency scaling decisions**: On platforms where hw_pressure feeds into cpufreq decisions (via `cpufreq_update_util()`), a corrupted hw_pressure signal could lead to inappropriate frequency selections, potentially causing performance degradation or excessive power consumption. The `others_have_blocked()` check in `__update_blocked_others()` also depends on the PELT signal being properly maintained; corruption could cause unnecessary repeated blocked load updates.

While this bug does not cause a kernel crash or oops, it degrades the quality of hardware pressure tracking on platforms that rely on it (primarily ARM/ARM64 systems with thermal throttling, but also x86 systems with `CONFIG_SCHED_HW_PRESSURE` enabled). The severity depends on the workload and platform: on systems with active thermal throttling and frequency invariance, the scheduling impact could be significant.

## Fix Summary

The fix is a one-line change in `__update_blocked_others()` in `kernel/sched/fair.c`. It replaces the `now` variable (which holds `rq_clock_pelt(rq)`) with an explicit call to `rq_clock_task(rq)` for the `update_hw_load_avg()` call:

```c
/* Before (buggy): */
decayed = update_rt_rq_load_avg(now, rq, curr_class == &rt_sched_class) |
          update_dl_rq_load_avg(now, rq, curr_class == &dl_sched_class) |
          update_hw_load_avg(now, rq, hw_pressure) |
          update_irq_load_avg(rq, 0);

/* After (fixed): */
/* hw_pressure doesn't care about invariance */
decayed = update_rt_rq_load_avg(now, rq, curr_class == &rt_sched_class) |
          update_dl_rq_load_avg(now, rq, curr_class == &dl_sched_class) |
          update_hw_load_avg(rq_clock_task(rq), rq, hw_pressure) |
          update_irq_load_avg(rq, 0);
```

This ensures that both call sites — `sched_tick()` and `__update_blocked_others()` — pass `rq_clock_task(rq)` to `update_hw_load_avg()`. The `rq_clock_task()` clock is the correct choice for hw_pressure because, as the added comment explains, "hw_pressure doesn't care about invariance." Unlike CFS and RT/DL PELT signals that need the frequency-invariant `rq_clock_pelt()` to properly account for reduced CPU compute capacity, the hw_pressure signal already represents a capacity delta directly and does not need the PELT clock's capacity scaling. Using the plain task clock ensures `last_update_time` always advances monotonically and consistently, eliminating both the negative-delta and inflated-delta scenarios.

The fix is correct and complete because: (a) the original code before commit `97450eb90965` used `rq_clock_hw()` which was derived from `rq_clock_task()`, so reverting to `rq_clock_task()` restores the original clock domain; (b) RT and DL signals are unaffected as they continue to use `rq_clock_pelt()` consistently in both call paths; and (c) the comment documents the design rationale to prevent future regressions.

## Triggering Conditions

The bug requires the following conditions to manifest:

1. **CONFIG_SCHED_HW_PRESSURE must be enabled**: If this config option is disabled, `update_hw_load_avg()` is a no-op stub and the bug cannot trigger. On ARM/ARM64 platforms, this is typically enabled. On x86, it depends on the distribution's kernel config.

2. **CPU running at reduced capacity**: The gap between `rq_clock_pelt()` and `rq_clock_task()` only grows when the CPU is running at reduced capacity (either reduced frequency via `arch_scale_freq_capacity()` or reduced compute capacity via `arch_scale_cpu_capacity()`). The `update_rq_clock_pelt()` function scales `clock_pelt` by `cap_scale(delta, arch_scale_cpu_capacity(cpu_of(rq)))`, so at half capacity, pelt clock advances at half the rate. The lower the capacity, the faster the clocks diverge. At full capacity (scale=1024), `clock_pelt` tracks `clock_task` closely and the bug has minimal impact.

3. **CPU must be busy (not idle)**: When the CPU goes idle, `_update_idle_rq_clock_pelt()` syncs `clock_pelt` back to `clock_task`, closing the gap. The bug requires the CPU to remain busy across multiple ticks so the clock gap accumulates. Even a brief idle period resets the divergence.

4. **Both code paths must execute**: The `sched_tick()` path runs on every timer tick for the local CPU. The `__update_blocked_others()` path runs during load balancing softirq processing, specifically from `run_rebalance_domains()` → `sched_balance_update_blocked_averages()`. This typically happens on every load balancing interval (a few milliseconds). For the bug to produce observable effects, both paths must update `rq->avg_hw` in alternating order within a short time window, so that the clock domain mismatch creates detectable delta anomalies.

5. **Kernel version v6.10-rc1 through v6.11**: The bug was introduced in v6.10-rc1 by commit `97450eb90965` and fixed in the v6.12-rc1 merge window by the fix commit. Any kernel in this range with `CONFIG_SCHED_HW_PRESSURE` enabled and CPU frequency/capacity scaling active will exhibit the bug.

The bug is deterministic given the above conditions — it is not a race condition but a systematic clock mismatch that corrupts the PELT signal on every alternation between the two code paths. The probability of triggering is very high on platforms with active thermal management and reduced CPU capacity.

## Reproduce Strategy (kSTEP)

The strategy to reproduce this bug in kSTEP focuses on creating a divergence between `rq_clock_pelt()` and `rq_clock_task()`, then observing the inconsistent `last_update_time` values in `rq->avg_hw` as both `sched_tick()` and `__update_blocked_others()` update the hw_pressure PELT signal.

### Prerequisites and Configuration

The kernel must be built with `CONFIG_SCHED_HW_PRESSURE=y` and `CONFIG_SMP=y`. The QEMU instance should be configured with at least 2 CPUs. CPU 0 is reserved for the driver; the test will run on CPU 1. The driver should verify at startup that `CONFIG_SCHED_HW_PRESSURE` is enabled (e.g., by checking that `update_hw_load_avg` is not a no-op stub, or by directly reading the symbol).

### Step-by-Step Driver Plan

1. **Setup topology and CPU capacity**: In the `setup()` function, call `kstep_cpu_set_capacity(1, 512)` to set CPU 1 to half capacity. This causes `update_rq_clock_pelt()` to scale `clock_pelt` by 0.5 on CPU 1, creating the necessary divergence between `rq_clock_pelt()` and `rq_clock_task()`.

2. **Create a busy task on CPU 1**: Create a CFS task with `kstep_task_create()` and pin it to CPU 1 with `kstep_task_pin(p, 1, 2)`. Wake the task with `kstep_task_wakeup(p)`. This keeps CPU 1 busy, preventing the idle sync that would close the clock gap.

3. **Run initial ticks to establish clock divergence**: Call `kstep_tick_repeat(20)` to advance 20 ticks. At half capacity, after 20 ticks, `rq_clock_pelt()` on CPU 1 should be approximately 10ms behind `rq_clock_task()` (assuming 1ms ticks). This establishes a significant gap.

4. **Instrument observation via on_tick_end callback or direct reads**: Use `KSYM_IMPORT` to access the `rq->avg_hw` sched_avg structure. After each tick, use `cpu_rq(1)` to get the run queue, then read:
   - `rq->avg_hw.last_update_time` — the last clock value used to update hw_pressure
   - `rq_clock_task(rq)` — the current task clock
   - `rq_clock_pelt(rq)` — the current PELT clock
   Log these values after each tick.

5. **Trigger blocked load update**: The blocked load update path (`__update_blocked_others()`) fires during load balancing softirq. This can be triggered by:
   - Having CPU 0 (or another CPU) go idle, which triggers `sched_balance_newidle()` → `sched_balance_update_blocked_averages()`
   - Alternatively, the periodic load balancing softirq will fire naturally after ticks
   Use the `on_sched_softirq_end` callback to observe when blocked load updates complete. After the softirq fires, immediately re-read `rq->avg_hw.last_update_time` to see if it changed clock domains.

6. **Detection logic — Primary method**: After each tick, compute two deltas:
   - `delta_task = rq_clock_task(rq) - avg_hw.last_update_time`
   - `delta_pelt = rq_clock_pelt(rq) - avg_hw.last_update_time`
   
   On the **buggy kernel**: After `sched_tick()` updates hw_pressure using `rq_clock_task()`, `delta_task` will be small (close to 0) and `delta_pelt` will be large (reflecting the clock gap). Then after `__update_blocked_others()` runs using `rq_clock_pelt()`, `last_update_time` will jump back to the pelt domain, making `delta_pelt` small and `delta_task` large. This oscillation is the signature of the bug.
   
   On the **fixed kernel**: `last_update_time` always stays in the `rq_clock_task()` domain, so `delta_task` is always small and stable.

7. **Detection logic — Alternative method (negative delta observation)**: Track `last_update_time` across consecutive observations. If `last_update_time` ever decreases between two observations (when observed from the same point in the tick cycle), the bug has manifested — it means `__update_blocked_others()` reset `last_update_time` backwards to the pelt clock domain after `sched_tick()` advanced it in the task clock domain.

8. **Pass/fail criteria**:
   - **PASS (bug reproduced on buggy kernel)**: `last_update_time` is observed to oscillate between clock domains, OR a backward jump in `last_update_time` is detected, OR the gap between `rq_clock_task(rq)` and `avg_hw.last_update_time` exceeds the expected single-tick delta by more than 2x after a blocked load update.
   - **PASS (bug absent on fixed kernel)**: `last_update_time` consistently tracks `rq_clock_task(rq)` and never exhibits backward jumps or oscillation.
   - **FAIL**: No divergence observed (possibly CONFIG_SCHED_HW_PRESSURE is disabled or CPU capacity was not reduced).

### kSTEP Changes Needed

This driver may require minor kSTEP additions:
- **Accessing `rq->avg_hw`**: The driver needs to read `rq->avg_hw.last_update_time`. This should be accessible via `cpu_rq(cpu)->avg_hw` using the internal scheduler headers (`sched.h`). If `sched_avg` is not directly exposed, `KSYM_IMPORT` may be needed for helper functions, or the internal.h include path may suffice.
- **Accessing `rq_clock_pelt()`**: This is defined as a `static inline` in `pelt.h`, so it should be callable from the driver if the header is included. If not, the driver can compute it manually from `rq->clock_pelt` and `rq->lost_idle_time`.
- **Forcing blocked load updates**: If natural load balancing softirq timing is insufficient, the driver may need to explicitly trigger `sched_balance_update_blocked_averages()` for the target CPU. This could be done by importing and calling the function via `KSYM_IMPORT(sched_balance_update_blocked_averages)`, or by creating conditions (idle CPUs) that naturally trigger it.

### Expected Behavior Summary

| Observation | Buggy Kernel | Fixed Kernel |
|---|---|---|
| `last_update_time` clock domain | Oscillates between `rq_clock_task` and `rq_clock_pelt` | Always `rq_clock_task` |
| Backward jumps in `last_update_time` | Yes, after blocked load update | Never |
| Gap: `rq_clock_task(rq) - last_update_time` | Large after blocked update (~10ms+ at half capacity) | Small (≤1 tick period) |
| PELT decay of hw_pressure | Excessive and erratic | Smooth and correct |
