# PELT: Incorrect cfs_rq_clock_pelt() for Throttled cfs_rq Due to Clock Domain Mismatch

**Commit:** `64eaf50731ac0a8c76ce2fedd50ef6652aabc5ff`
**Affected files:** `kernel/sched/fair.c`, `kernel/sched/pelt.h`, `kernel/sched/sched.h`
**Fixed in:** v5.19-rc1
**Buggy since:** v5.1-rc1 (introduced by commit `23127296889f` — "sched/fair: Update scale invariance of PELT")

## Bug Description

The Per-Entity Load Tracking (PELT) subsystem in the Linux scheduler uses a specialized clock called `rq_clock_pelt()` to drive load/utilization signal accumulation. This clock was introduced by commit `23127296889f` ("sched/fair: Update scale invariance of PELT") in v5.1 to make PELT signals frequency-invariant. Unlike `rq_clock_task()` which advances at wall-clock rate (minus IRQ time), `rq_clock_pelt()` advances at a rate scaled by the current CPU frequency and micro-architecture efficiency. When a CPU runs at half its maximum frequency, `rq_clock_pelt()` advances at half the rate of `rq_clock_task()`, making the two clocks diverge significantly over time.

When CFS bandwidth control is enabled and a cfs_rq is throttled (because the task group has exceeded its CPU quota), the scheduler needs to "freeze" the PELT clock for that cfs_rq during the throttled period. This is done by recording the clock at throttle time (`throttled_clock_task`) and accumulating total throttled time (`throttled_clock_task_time`). The `cfs_rq_clock_pelt()` function then subtracts the accumulated throttled time from the current clock to get an effective PELT time that excludes throttled intervals.

The bug is that when commit `23127296889f` switched the PELT system from using `rq_clock_task()` to `rq_clock_pelt()`, it failed to also update the throttled time accounting in the CFS bandwidth code. The fields `throttled_clock_task` and `throttled_clock_task_time` continued to be computed using `rq_clock_task()`, while `cfs_rq_clock_pelt()` subtracted these task-domain values from pelt-domain values. This cross-domain subtraction produces incorrect results whenever `rq_clock_pelt()` and `rq_clock_task()` differ — i.e., whenever the CPU is not running at maximum capacity.

## Root Cause

The root cause is a clock domain mismatch between the numerator and denominator in the `cfs_rq_clock_pelt()` calculation for throttled cfs_rqs.

In the buggy code, `cfs_rq_clock_pelt()` in `kernel/sched/pelt.h` is defined as:

```c
static inline u64 cfs_rq_clock_pelt(struct cfs_rq *cfs_rq)
{
    if (unlikely(cfs_rq->throttle_count))
        return cfs_rq->throttled_clock_task - cfs_rq->throttled_clock_task_time;

    return rq_clock_pelt(rq_of(cfs_rq)) - cfs_rq->throttled_clock_task_time;
}
```

There are two problems here:

**Problem 1 (throttled path):** When the cfs_rq is currently throttled (`throttle_count > 0`), it returns `throttled_clock_task - throttled_clock_task_time`. Both values are in the `rq_clock_task()` domain. While they are at least internally consistent, this value is then used by PELT update functions that expect a `rq_clock_pelt()`-domain timestamp. The callers (`___update_load_sum()` etc.) compute deltas against `se->avg.last_update_time`, which was previously set using `cfs_rq_clock_pelt()` in the pelt domain. The mismatch in time domains produces incorrect deltas and thus incorrect PELT signal updates.

**Problem 2 (unthrottled path):** When the cfs_rq is not currently throttled but has been throttled in the past, it returns `rq_clock_pelt(rq) - throttled_clock_task_time`. Here, `rq_clock_pelt()` is in the pelt domain, but `throttled_clock_task_time` is in the task-clock domain. The subtraction of values from two different time domains produces a nonsensical result. If the CPU was running at 50% capacity during the throttled period, `rq_clock_task()` would have advanced twice as fast as `rq_clock_pelt()`, so `throttled_clock_task_time` would be roughly double what the equivalent pelt-domain throttled time would be, causing `cfs_rq_clock_pelt()` to return a value that is too small or even underflow.

The three functions that record throttled time are in `fair.c`:

1. **`tg_throttle_down()`** — records the timestamp when throttling starts:
   ```c
   cfs_rq->throttled_clock_task = rq_clock_task(rq);  // BUG: should be rq_clock_pelt(rq)
   ```

2. **`tg_unthrottle_up()`** — accumulates the duration of the throttled interval:
   ```c
   cfs_rq->throttled_clock_task_time += rq_clock_task(rq) - cfs_rq->throttled_clock_task;
   // BUG: should be rq_clock_pelt(rq) - cfs_rq->throttled_clock_pelt
   ```

3. **`sync_throttle()`** — initializes the throttle clock for a newly created child cfs_rq:
   ```c
   cfs_rq->throttled_clock_task = rq_clock_task(cpu_rq(cpu));  // BUG: should be rq_clock_pelt()
   ```

All three functions use `rq_clock_task()` when they should use `rq_clock_pelt()` to stay in the same clock domain as `rq_clock_pelt()` used by `cfs_rq_clock_pelt()`.

## Consequence

The consequence of this bug is incorrect PELT (Per-Entity Load Tracking) signal computation for entities within cgroups that use CFS bandwidth control. Specifically, `util_avg`, `load_avg`, and `runnable_avg` values for scheduling entities in throttled task groups will be miscalculated.

The severity depends on how much `rq_clock_pelt()` and `rq_clock_task()` diverge, which is a function of CPU frequency. On systems where CPUs always run at maximum frequency (e.g., performance governor on desktop systems), the two clocks are identical and the bug is latent. On systems with frequency scaling (e.g., mobile/embedded platforms using `schedutil` governor, big.LITTLE/DynamIQ ARM platforms), the divergence can be significant. At 50% maximum frequency, `rq_clock_pelt()` advances at half the rate of `rq_clock_task()`, meaning the throttled time accumulator (`throttled_clock_task_time`) will over-count by a factor of 2 relative to the pelt clock. This causes `cfs_rq_clock_pelt()` to return values that are too small, which in turn makes PELT signals appear to decay too slowly (or not at all) during unthrottled periods.

The downstream effects include: (1) incorrect load balancing decisions because `load_avg` values are wrong for bandwidth-throttled cgroups; (2) incorrect CPU frequency selection by the `schedutil` governor because `util_avg` signals are inaccurate, potentially causing the CPU to run at too high or too low a frequency; (3) incorrect energy-aware scheduling (EAS) decisions on heterogeneous platforms where PELT utilization drives task placement. In extreme cases where `throttled_clock_task_time` significantly exceeds the pelt-domain equivalent, `cfs_rq_clock_pelt()` could underflow (wrapping to a very large u64), which would cause wildly incorrect time deltas in PELT update calculations, leading to corrupted averages. There is no crash or panic — this is a silent data corruption of scheduling signals.

## Fix Summary

The fix is straightforward: replace all uses of `rq_clock_task()` with `rq_clock_pelt()` in the CFS bandwidth throttle/unthrottle time accounting, and rename the fields to reflect their new semantics.

In `kernel/sched/sched.h`, the struct `cfs_rq` fields are renamed from `throttled_clock_task` and `throttled_clock_task_time` to `throttled_clock_pelt` and `throttled_clock_pelt_time`. This is a purely cosmetic rename, but it makes the clock domain explicit in the variable names, preventing future confusion.

In `kernel/sched/fair.c`, three functions are updated:
- `tg_throttle_down()`: `cfs_rq->throttled_clock_pelt = rq_clock_pelt(rq)` (was `rq_clock_task(rq)`)
- `tg_unthrottle_up()`: `cfs_rq->throttled_clock_pelt_time += rq_clock_pelt(rq) - cfs_rq->throttled_clock_pelt` (was `rq_clock_task(rq) - cfs_rq->throttled_clock_task`)
- `sync_throttle()`: `cfs_rq->throttled_clock_pelt = rq_clock_pelt(cpu_rq(cpu))` (was `rq_clock_task(cpu_rq(cpu))`)

In `kernel/sched/pelt.h`, `cfs_rq_clock_pelt()` is updated to use the renamed fields:
```c
if (unlikely(cfs_rq->throttle_count))
    return cfs_rq->throttled_clock_pelt - cfs_rq->throttled_clock_pelt_time;
return rq_clock_pelt(rq_of(cfs_rq)) - cfs_rq->throttled_clock_pelt_time;
```

The fix is correct and complete because it ensures all values in the `cfs_rq_clock_pelt()` computation are in the same clock domain (`rq_clock_pelt`). The accumulated throttled time is now measured in pelt-domain nanoseconds, which means the subtraction from `rq_clock_pelt()` produces a valid pelt-domain timestamp for PELT signal updates.

## Triggering Conditions

The following conditions are needed to trigger this bug:

1. **CFS bandwidth control must be enabled** (`CONFIG_CFS_BANDWIDTH=y`, which requires `CONFIG_FAIR_GROUP_SCHED=y` and `CONFIG_CGROUP_SCHED=y`). A task group must have a CPU quota configured (e.g., `cpu.cfs_quota_us` in cgroup v1, or `cpu.max` in cgroup v2).

2. **The CPU frequency must differ from maximum capacity.** If the CPU is running at maximum frequency, `rq_clock_pelt()` and `rq_clock_task()` are identical and the bug is latent. The frequency can be lower due to frequency scaling (schedutil, ondemand, powersave governor), or due to micro-architecture capacity differences on big.LITTLE/DynamIQ platforms. The greater the deviation from maximum capacity, the more pronounced the bug becomes.

3. **A task in the bandwidth-limited cgroup must be running on the CPU** so that the cfs_rq gets throttled when the quota is exhausted. After throttling, the task must eventually become unthrottled (quota refilled at period boundary) so that PELT updates resume with the corrupted clock.

4. **Multiple throttle/unthrottle cycles amplify the error.** Each cycle accumulates more incorrectly-measured throttled time in `throttled_clock_task_time`, progressively worsening the clock domain mismatch. A single throttle/unthrottle cycle at low CPU frequency is sufficient to demonstrate the bug, but the error compounds over successive cycles.

5. **No specific timing race is required.** This is a deterministic logic error, not a race condition. Whenever a throttle/unthrottle cycle completes on a CPU running below maximum frequency, the accumulated throttled time will be wrong.

The bug is highly reproducible given the above conditions: create a cgroup with a restrictive bandwidth limit, run a CPU-intensive task in it on a CPU with reduced frequency scaling, and observe the PELT signals after throttle/unthrottle cycles.

## Reproduce Strategy (kSTEP)

The strategy is to demonstrate that `cfs_rq_clock_pelt()` returns incorrect values for a throttled cfs_rq when the CPU frequency is below maximum, by comparing the PELT clock and utilization signals between the buggy and fixed kernels.

### Step-by-step plan:

1. **Topology setup:** Configure QEMU with 2 CPUs (CPU 0 for the driver, CPU 1 for the workload). No special topology is needed beyond SMP.

2. **Set CPU frequency scaling:** Use `kstep_cpu_set_freq(1, 512)` to set CPU 1 to 50% of maximum frequency (capacity scale 512 out of 1024). This creates the divergence between `rq_clock_pelt()` and `rq_clock_task()` that is necessary to expose the bug. At 50% frequency, `rq_clock_pelt()` advances at approximately half the rate of `rq_clock_task()`.

3. **Create a bandwidth-limited cgroup:** Use `kstep_cgroup_create("bw_test")` to create a test cgroup. Then set a CFS bandwidth quota. kSTEP does not currently have a `kstep_cgroup_set_bandwidth()` API, so a **minor kSTEP extension** is needed. The extension should write to the cgroup's `cpu.max` file (cgroup v2) or `cpu.cfs_quota_us` / `cpu.cfs_period_us` files (cgroup v1) from kernel space. Alternatively, the quota can be set by directly manipulating the `tg_cfs_bandwidth()` structure via `KSYM_IMPORT` and internal access. Set a tight quota (e.g., 5ms quota per 100ms period) so the task gets throttled quickly.

4. **Create and pin a task:** Use `kstep_task_create()` to create a CFS task, pin it to CPU 1 with `kstep_task_pin(task, 1, 2)`, and add it to the cgroup with `kstep_cgroup_add_task("bw_test", task->pid)`.

5. **Run the task through throttle/unthrottle cycles:** Wake up the task with `kstep_task_wakeup(task)`. Advance time with `kstep_tick_repeat(N)` to let the task run, consume its quota, get throttled, wait for quota refill, get unthrottled, and repeat. Run enough ticks for at least 2-3 complete throttle/unthrottle cycles (e.g., 200+ ticks at HZ=1000 for a 100ms period).

6. **Observe the PELT clock during throttled and unthrottled states:** Using the `on_tick_begin` or `on_sched_softirq_end` callback, read and log the following values on CPU 1 at each tick:
   - `rq_clock_pelt(rq)` — the pelt-domain clock
   - `rq_clock_task(rq)` — the task-domain clock
   - `cfs_rq->throttled_clock_pelt` (or `throttled_clock_task` on buggy kernel)
   - `cfs_rq->throttled_clock_pelt_time` (or `throttled_clock_task_time` on buggy kernel)
   - `cfs_rq_clock_pelt(cfs_rq)` — the computed PELT clock for the cgroup's cfs_rq
   - `cfs_rq->throttle_count` — to know if currently throttled
   - `cfs_rq->avg.util_avg` and `cfs_rq->avg.load_avg` — downstream PELT signals

7. **Detection criteria (pass/fail):**
   - **On the buggy kernel:** After a throttle/unthrottle cycle, `cfs_rq_clock_pelt()` will return an incorrect value because it subtracts `throttled_clock_task_time` (task-domain, larger value at 50% freq) from `rq_clock_pelt()` (pelt-domain, smaller value). The returned PELT clock will be too small or may underflow. Specifically, if the task was throttled for duration `T` wall-clock time, `throttled_clock_task_time` will accumulate approximately `T` nanoseconds (task-clock rate), while the pelt-domain equivalent should be approximately `T * 0.5` nanoseconds. The difference `T - T*0.5 = T*0.5` nanoseconds of extra subtraction will make `cfs_rq_clock_pelt()` lag behind the correct value, growing with each cycle.
   
   - **On the fixed kernel:** `cfs_rq_clock_pelt()` correctly subtracts pelt-domain throttled time from pelt-domain current time. The returned value accurately reflects time-excluding-throttled-intervals in the pelt domain. PELT signals (`util_avg`, `load_avg`) will correctly track the task's actual utilization.
   
   - **Detection method:** After 2+ throttle/unthrottle cycles, compute the expected `cfs_rq_clock_pelt()` value: it should equal `rq_clock_pelt(rq) - total_pelt_throttled_time`. On the buggy kernel, compare `rq_clock_pelt(rq) - throttled_clock_task_time` against the expected value; the difference will be proportional to `(1 - freq_scale) * total_throttled_wall_time`. Use `kstep_fail()` if the discrepancy exceeds a threshold (e.g., 1ms), and `kstep_pass()` otherwise.

8. **Alternative simpler detection:** Instead of computing exact expected values, use `KSYM_IMPORT` to access both `rq_clock_pelt` and `rq_clock_task` functions from within the driver. After an unthrottle event, check whether `cfs_rq_clock_pelt(cfs_rq)` is strictly less than `rq_clock_pelt(rq) - expected_pelt_throttled_time`. On the buggy kernel, the value will be too small due to over-subtraction. A simpler heuristic: check that `cfs_rq_clock_pelt(cfs_rq)` is monotonically increasing across tick observations; on the buggy kernel it may stall or jump backwards after unthrottle.

### kSTEP Extensions Needed:

A minor extension is needed to set CFS bandwidth parameters on a cgroup. This could be implemented as:
- `kstep_cgroup_set_bandwidth(const char *name, u64 quota_us, u64 period_us)` — writes to the cgroup's cpu.max or cpu.cfs_quota_us/cpu.cfs_period_us.
- Alternatively, since kSTEP provides access to internal scheduler structures through `internal.h`, the driver could directly manipulate `tg_cfs_bandwidth(cfs_rq->tg)` to set `quota` and `period`, then start the bandwidth timer. This avoids needing a new kSTEP API function.

### Expected Kernel Versions:

The buggy kernel must be between v5.1 (when `23127296889f` was merged) and v5.19-rc1 (when this fix was merged). Use `checkout_linux.py 64eaf50731ac~1 pelt_clock_buggy` for the buggy kernel and `checkout_linux.py 64eaf50731ac pelt_clock_fixed` for the fixed kernel. The kernel must have `CONFIG_CFS_BANDWIDTH=y`, `CONFIG_FAIR_GROUP_SCHED=y`, `CONFIG_SMP=y`, and `CONFIG_CGROUP_SCHED=y` enabled.
