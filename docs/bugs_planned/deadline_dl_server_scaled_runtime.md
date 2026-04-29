# Deadline: dl_server Runtime Incorrectly Scaled by Frequency/Capacity on Asymmetric CPUs

**Commit:** `fc975cfb36393db1db517fbbe366e550bcdcff14`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.16-rc5
**Buggy since:** v6.12-rc1 (commit `a110a81c52a9` "sched/deadline: Deferrable dl server")

## Bug Description

The Linux kernel's SCHED_DEADLINE subsystem provides a "dl_server" mechanism (the `fair_server`) that gives CFS tasks a minimum guaranteed bandwidth even in the presence of RT tasks. The dl_server is configured with a runtime/period budget (default: 50ms per 1000ms period). During each period, the dl_server is allowed to run for up to `runtime` nanoseconds to service CFS tasks; once this budget is exhausted, the dl_server is throttled and RT tasks can run unimpeded for the remainder of the period.

When the dl_server was made deferrable in commit `a110a81c52a9`, the runtime accounting in `update_curr_dl_se()` applied frequency and CPU capacity scale-invariant adjustments to the dl_server's runtime consumption — the same scaling that is correctly used for regular SCHED_DEADLINE tasks. For regular DL tasks, this scaling normalizes execution time across CPUs of different speeds so that a task's deadline parameters behave consistently regardless of which CPU it runs on. However, for the fair dl_server this scaling is semantically wrong: the dl_server's purpose is to limit the amount of *wall-clock time* that CFS tasks can block RT tasks, and that limit must be in real time, not in capacity-normalized time.

On big.LITTLE (heterogeneous) systems where LITTLE CPUs have significantly lower frequency and capacity than big CPUs, this scaling dramatically reduces the runtime that is *accounted* against the dl_server's budget per unit of real time elapsed. For example, on a LITTLE CPU with frequency scale factor 100 (out of 1024) and capacity scale factor 50 (out of 1024), a delta of 50ms of real execution time is scaled down to approximately 238 microseconds of accounted runtime. This means the dl_server's 50ms budget takes over 10 seconds of real time to exhaust on such a CPU, during which RT tasks are blocked from running.

This was discovered by MediaTek engineers testing a 6.12-based kernel on a big.LITTLE Android system, and independently confirmed by John Stultz at Google using `cyclictest -t -a -p99 -m` on Android devices, where RT priority-99 threads experienced delays of 100ms to multiple seconds.

## Root Cause

The root cause is in two functions in `kernel/sched/deadline.c` that both incorrectly apply frequency/capacity scaling to the fair dl_server's runtime accounting.

**In `update_curr_dl_se()`** (the main runtime accounting function for all DL entities):

```c
/* Buggy code: */
scaled_delta_exec = dl_scaled_delta_exec(rq, dl_se, delta_exec);
dl_se->runtime -= scaled_delta_exec;
```

The function `dl_scaled_delta_exec()` applies two scaling operations:
1. Frequency scale-invariance: `scaled_delta_exec = cap_scale(delta_exec, arch_scale_freq_capacity(cpu))` — this scales by `delta_exec * freq_scale >> 10`
2. CPU capacity scale-invariance: `scaled_delta_exec = cap_scale(scaled_delta_exec, arch_scale_cpu_capacity(cpu))` — this further scales by `scaled * cpu_scale >> 10`

The `cap_scale` macro performs `(x * scale) >> SCHED_CAPACITY_SHIFT` where `SCHED_CAPACITY_SHIFT = 10` and `SCHED_CAPACITY_SCALE = 1024`. On a big CPU where both scale factors are 1024, the result is approximately equal to `delta_exec` (identity transform). But on a LITTLE CPU where both factors are small, the result is drastically smaller than `delta_exec`.

Consider the concrete example from the commit message: frequency scale = 100, capacity scale = 50:
- Input: `delta_exec = 50,000,000 ns` (50ms of real execution)
- After frequency scaling: `50,000,000 * 100 >> 10 = 4,882,812 ns`
- After capacity scaling: `4,882,812 * 50 >> 10 = 238,418 ns`
- Result: Only 238μs is subtracted from the dl_server's 50ms budget after 50ms of real time

This 209:1 ratio means the dl_server can run for `50ms * 209 ≈ 10.4 seconds` of real time before its budget is exhausted. During all of this time, RT tasks waiting to run on that CPU are blocked.

**In `dl_server_update_idle_time()`** (which accounts idle time against the deferred dl_server's budget):

```c
/* Buggy code: */
scaled_delta_exec = dl_scaled_delta_exec(rq, &rq->fair_server, delta_exec);
rq->fair_server.runtime -= scaled_delta_exec;
```

The same incorrect scaling is applied when accounting idle time against the fair server. This function is called to deduct idle time from the fair server's budget when the CPU is idle during the dl_server's deferred period, preventing the dl_server from penalizing RT tasks for time that nobody was using. With the incorrect scaling, the idle time deduction is also too small, compounding the problem.

## Consequence

The observable impact is **severe RT task latency on LITTLE CPUs** of heterogeneous (big.LITTLE / DynamIQ) systems. RT tasks (SCHED_FIFO, SCHED_RR) that should only be blocked by the dl_server for at most 50ms per second can instead be blocked for many seconds — over 10 seconds in the example configuration, and potentially even worse on hardware with more extreme asymmetry.

On Android devices running a 6.12-based kernel, this manifests as `cyclictest` (a standard RT latency measurement tool) reporting latencies of 100ms to multiple seconds for priority-99 RT threads. This is catastrophic for real-time workloads — audio processing, sensor fusion, motor control, and other latency-sensitive tasks can miss their deadlines by orders of magnitude. The effect is worst on the smallest/slowest CPU cores, which are precisely the ones most likely to be running background tasks alongside occasional RT work.

The bug affects all systems with asymmetric CPU capacity/frequency where the dl_server is active (the default configuration). It does not affect symmetric multiprocessing systems where all CPUs have scale factors of 1024, since the scaling is an identity transform in that case. The severity scales with the asymmetry: larger differences between big and LITTLE CPU capacities produce worse RT latency. The hardware configuration described in the commit (freq=100, cap=50) is representative of typical ARM big.LITTLE platforms.

## Fix Summary

The fix is straightforward: skip the frequency/capacity scaling for dl_server entities, applying it only to regular SCHED_DEADLINE tasks.

**In `update_curr_dl_se()`**, the unconditional call to `dl_scaled_delta_exec()` is replaced with a conditional:

```c
/* Fixed code: */
scaled_delta_exec = delta_exec;
if (!dl_server(dl_se))
    scaled_delta_exec = dl_scaled_delta_exec(rq, dl_se, delta_exec);
```

When the entity is a dl_server (checked via the `dl_server()` helper, which tests `dl_se->dl_server`), the raw `delta_exec` is used directly — the real wall-clock time elapsed. For regular DL tasks, the existing frequency/capacity scaling continues to be applied correctly.

**In `dl_server_update_idle_time()`**, the scaling is removed entirely since this function is only ever called for the fair_server (a dl_server by definition):

```c
/* Fixed code: */
rq->fair_server.runtime -= delta_exec;
```

The `scaled_delta_exec` local variable is also removed from the function since it is no longer needed.

This fix is correct because the dl_server's purpose is to bound the wall-clock time that CFS tasks can delay RT tasks. The 50ms/1s default budget means "CFS tasks may run for at most 50ms of real time per second." Applying capacity scaling would change the semantics to "CFS tasks may run for at most 50ms of *normalized* time per second," which on a slow CPU translates to far more than 50ms of real time. The original `dl_scaled_delta_exec()` scaling is meaningful for regular SCHED_DEADLINE tasks whose parameters are specified in terms of abstract "execution capacity" and need normalization across heterogeneous CPUs, but is incorrect for the dl_server's fixed wall-clock budget.

## Triggering Conditions

The following conditions are needed to trigger this bug:

1. **Kernel version**: Any kernel from v6.12-rc1 (containing `a110a81c52a9`) through v6.16-rc4 (just before the fix). The bug is present in all stable kernels v6.12.x through v6.15.x.

2. **Heterogeneous CPU topology**: The system must have CPUs with different capacity and/or frequency scale factors. Specifically, at least one CPU must have `arch_scale_freq_capacity()` and/or `arch_scale_cpu_capacity()` significantly below 1024 (the maximum). The more asymmetric the CPUs, the more pronounced the bug.

3. **dl_server active**: The fair dl_server must be enabled (the default). It starts with `dl_runtime = 50ms` and `dl_period = 1000ms`. If the dl_server has been explicitly disabled (`dl_runtime = 0`), the bug cannot trigger.

4. **RT task running on a LITTLE CPU**: At least one SCHED_FIFO or SCHED_RR task must be runnable on a CPU with low capacity/frequency scale factors. The RT task will be blocked by the dl_server for far longer than the intended 50ms.

5. **CFS tasks present**: There must be CFS tasks runnable on the same CPU, so that the dl_server activates and serves them. Without CFS tasks, the dl_server does not consume runtime.

6. **No special kernel config required**: The bug triggers with default scheduler configuration. `CONFIG_SMP=y` and frequency/capacity invariance support (e.g., `CONFIG_ARCH_SCALE_FREQ_CAPACITY`) are needed, but these are standard on ARM platforms.

The bug is **deterministic** — it always occurs on affected hardware configurations. There is no race condition or timing-dependent window. Every tick where the dl_server runs on a low-capacity CPU will under-account its runtime consumption. The magnitude of the delay depends on the specific scale factors of the CPU but is always proportional to the `1024 / (freq_scale * cap_scale >> 10)` ratio.

## Reproduce Strategy (kSTEP)

The strategy is to simulate an asymmetric CPU topology using kSTEP's `kstep_cpu_set_freq()` and `kstep_cpu_set_capacity()` APIs, then demonstrate that the dl_server's runtime on a LITTLE CPU is consumed far too slowly (buggy kernel) versus at the correct rate (fixed kernel).

### QEMU Configuration

Configure QEMU with at least 2 CPUs. CPU 0 is reserved for the driver. CPU 1 will be the "LITTLE" CPU with reduced frequency and capacity.

### Setup Phase

1. **Set asymmetric CPU scales**: Use `kstep_cpu_set_freq(1, 100)` and `kstep_cpu_set_capacity(1, 50)` to make CPU 1 a "LITTLE" CPU with frequency scale 100/1024 and capacity scale 50/1024. These match the exact values described in the commit message.

2. **Import internal symbols**: Use `KSYM_IMPORT` to access `cpu_rq` for inspecting the fair_server state on CPU 1.

### Task Creation

1. Create an RT task: `rt_task = kstep_task_create()`, then `kstep_task_fifo(rt_task)` and `kstep_task_pin(rt_task, 1, 2)` to pin it to CPU 1.

2. Create a CFS task: `cfs_task = kstep_task_create()` and `kstep_task_pin(cfs_task, 1, 2)` to pin it to CPU 1.

### Execution Sequence

**Step 1**: Wake both tasks: `kstep_task_wakeup(rt_task)` and `kstep_task_wakeup(cfs_task)`. The RT task will be running on CPU 1 (highest priority). The CFS task will be enqueued, triggering `dl_server_start()` for `rq->fair_server`.

**Step 2**: Record the initial fair_server runtime. Access `cpu_rq(1)->fair_server.runtime` — it should be approximately 50,000,000 ns (50ms).

**Step 3**: Advance ticks to simulate time passing. The dl_server should begin its period and start accounting runtime. Use `kstep_tick_repeat(100)` to advance 100 ticks (100ms at default 1ms tick). During this time, when the dl_server is selected to run (giving the CFS task a turn), its runtime should be consumed.

**Step 4**: After some ticks, read `cpu_rq(1)->fair_server.runtime` again and compute how much runtime was consumed versus how much real time elapsed.

### Detection / Pass-Fail Criteria

The key observation is the **ratio** of real time elapsed to runtime consumed by the fair_server:

- **On the buggy kernel**: After 50ms of real execution under the dl_server, only ~238μs of runtime will have been subtracted from the fair_server's budget. The runtime remaining will be very close to the initial 50ms (e.g., 49.76ms remaining after 50ms of real execution). After 100 ticks (100ms), the fair_server will still have most of its budget remaining.

- **On the fixed kernel**: After 50ms of real execution under the dl_server, ~50ms of runtime will have been subtracted. The fair_server budget will be nearly or fully exhausted, and the server will be throttled.

Use the `on_tick_begin` callback to log the fair_server's runtime at each tick. After sufficient ticks:

```c
struct rq *rq1 = cpu_rq(1);
s64 remaining = rq1->fair_server.runtime;
```

**Pass condition**: After the dl_server has been active for at least one full period (1000 ticks), check that the fair_server runtime was exhausted within approximately 50ms of real wall-clock execution time (i.e., the `dl_throttled` flag is set after roughly 50 ticks of dl_server execution, not after thousands). More precisely, after 200 ticks (well beyond the 50ms budget), the fair_server should have been throttled at least once. Call `kstep_pass()`.

**Fail condition**: If after 200+ ticks the fair_server has still NOT been throttled (its runtime is still significantly positive, e.g., > 40,000,000), then the scaling bug is present and the dl_server is over-running its budget. Call `kstep_fail()`.

### Alternative Detection Approach

Instead of monitoring the runtime budget directly, observe the **impact on RT task scheduling**: use the `on_tick_begin` callback to check `cpu_rq(1)->curr` and track how long the CFS task runs continuously. On the buggy kernel, the CFS task (via dl_server) will run for far longer than 50ms before the RT task gets control back. On the fixed kernel, the dl_server will be throttled after ~50ms, allowing the RT task to run.

Track the number of consecutive ticks where `cpu_rq(1)->curr == cfs_task`. If this count exceeds a threshold (e.g., 100 ticks = 100ms, which is 2x the expected 50ms budget), the bug is present.

### kSTEP Extensions Needed

No extensions to the kSTEP framework are required. The existing APIs — `kstep_cpu_set_freq()`, `kstep_cpu_set_capacity()`, `kstep_task_create()`, `kstep_task_fifo()`, `kstep_task_pin()`, `kstep_task_wakeup()`, `kstep_tick_repeat()`, `KSYM_IMPORT` for `cpu_rq`, and the `on_tick_begin` callback — provide everything needed to simulate the heterogeneous CPU environment and observe the bug's effects.

The core mechanism is deterministic: setting low frequency/capacity scale factors on a CPU directly causes the dl_scaled_delta_exec() function to under-account the dl_server's runtime, and this can be observed by reading the fair_server's runtime field or by tracking task scheduling patterns. There are no race conditions or non-deterministic behaviors involved.
