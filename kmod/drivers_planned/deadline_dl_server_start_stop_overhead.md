# Deadline: dl_server start/stop overhead on frequent CFS task transitions

**Commit:** `cccb45d7c4295bbfeba616582d0249f2d21e6df5`
**Affected files:** `kernel/sched/deadline.c`, `kernel/sched/fair.c`, `include/linux/sched.h`
**Fixed in:** v6.17-rc1
**Buggy since:** v6.12-rc1 (introduced by commit `557a6bfc662c` "sched/fair: Add trivial fair server")

## Bug Description

The dl_server (deadline server) is a mechanism that provides CFS (fair scheduling class) tasks with bandwidth guarantees by wrapping them in a SCHED_DEADLINE entity. When a CFS task is enqueued on an empty runqueue (transitioning from 0 to 1 CFS tasks), `dl_server_start()` is called to activate the deadline server. When the last CFS task is dequeued (transitioning from 1 to 0 CFS tasks), `dl_server_stop()` is called to deactivate it. This start/stop cycle happens on every single 0→1 and 1→0 transition.

Chris Mason from Meta reported that commit `5f6bd380c7bd` ("sched/rt: Remove default bandwidth control"), which enabled the dl_server by default, caused a significant performance regression in the `schbench` benchmark. The workload mimics a high context-switch-rate scenario (using `schbench -L -m 4 -M auto -t 128 -n 0 -r 60`) where tasks rapidly wake up, do a small amount of work, and sleep again. This results in CPUs frequently oscillating between having 0 and 1 CFS tasks, hammering the `dl_server_start()`/`dl_server_stop()` path on every transition. Simply disabling dl_server entirely restored performance, confirming the overhead was in the start/stop operations.

The overhead of `dl_server_start()` is non-trivial: it calls `enqueue_dl_entity()` which involves inserting into the deadline runqueue's red-black tree, replenishing the deadline entity's parameters, and potentially triggering a reschedule. Similarly, `dl_server_stop()` calls `dequeue_dl_entity()` and `hrtimer_try_to_cancel()`. On Intel SPR systems, the regression was approximately 3% in RPS (requests per second); on Intel SKL systems it was approximately 6-7%. Peter Zijlstra's benchmark numbers showed v6.15 baseline at ~2891k RPS on SPR, which improved to ~3038k RPS after the fix — a meaningful recovery.

The bug existed in the design from when the trivial fair server was first introduced in v6.12-rc1 (commit `557a6bfc662c`), but only became a real problem when `5f6bd380c7bd` enabled it by default, making the dl_server active on all systems rather than being an opt-in feature.

## Root Cause

The root cause is that `dl_server_start()` and `dl_server_stop()` were called synchronously on every CFS runqueue occupancy transition (0→1 and 1→0), without any hysteresis or debouncing mechanism. This is fundamentally inefficient for workloads where tasks frequently wake up and sleep in rapid succession.

In the buggy code, `dl_server_stop()` was called from two locations in `kernel/sched/fair.c`:

1. **`dequeue_entities()`** (line ~7054 in the pre-fix code): When dequeuing a CFS task causes `rq->cfs.h_nr_queued` to drop from non-zero to zero, `dl_server_stop(&rq->fair_server)` is called immediately. The code checked `if (rq_h_nr_queued && !rq->cfs.h_nr_queued)` where `rq_h_nr_queued` was captured before the dequeue.

2. **`throttle_cfs_rq()`** (line ~5892 in the pre-fix code): When CFS bandwidth throttling removes all runnable CFS tasks from a runqueue, the same pattern `if (rq_h_nr_queued && !rq->cfs.h_nr_queued)` triggers `dl_server_stop()`.

And `dl_server_start()` was called from:

1. **`enqueue_task_fair()`** (line ~6926): When the first CFS task is enqueued on a runqueue that previously had none (`!rq_h_nr_queued && rq->cfs.h_nr_queued`).

2. **`unthrottle_cfs_rq()`** (line ~5991): When unthrottling restores CFS tasks to a previously empty runqueue.

The `dl_server_start()` function performs significant work: it checks if the server has already been initialized and if not, sets up a deadline entity with runtime=50ms, period=1000ms (5% bandwidth), calls `dl_server_apply_params()` and `setup_new_dl_entity()`. It then calls `enqueue_dl_entity(dl_se, ENQUEUE_WAKEUP)` to insert the server into the DL runqueue's red-black tree, and potentially calls `resched_curr()` if the server should preempt the current task. Conversely, `dl_server_stop()` calls `dequeue_dl_entity(dl_se, DEQUEUE_SLEEP)`, `hrtimer_try_to_cancel(&dl_se->dl_timer)`, and clears multiple flags. Each of these operations involves lock-protected data structure manipulations.

For workloads that rapidly cycle tasks through a CPU (e.g., schbench with many thread groups doing request-response patterns), a single CPU might see hundreds of thousands of 0→1→0 transitions per second. Each transition pair requires one `dl_server_start()` plus one `dl_server_stop()` call, making the aggregate overhead measurable at the benchmark level.

Additionally, in `__pick_task_dl()`, when the dl_server is picked but `server_pick_task()` returns NULL (no CFS task available at pick time), the code called `dl_server_active(dl_se)` and if true, would yield and update the server. This path was also exercised frequently in the racing scenario.

## Consequence

The primary consequence is a measurable performance degradation in workloads with high context-switch rates and frequent task wakeup/sleep cycles. The regression manifests as:

- **Reduced throughput**: On Intel SPR (Sapphire Rapids), approximately 3% reduction in schbench RPS (~2882k vs ~2975k baseline). On Intel SKL (Skylake), approximately 6-7% reduction (~1907k vs ~2040k baseline). These are significant regressions for production workloads. On PowerPC systems (5 cores, SMT8), testing by Shrikanth Hegde showed even more dramatic regressions — up to 54-61% reduction in RPS at higher thread counts (64-128 threads), with wakeup latencies increasing by 7-8x at the p50 percentile.

- **Increased latency**: The schbench wakeup latencies at the 99th percentile showed measurable increases due to the overhead of dl_server start/stop operations sitting in the critical enqueue/dequeue path. The dl_server operations are not free — they involve red-black tree insertions/deletions, hrtimer operations, and potential rescheduling IPIs.

- **Wasted CPU cycles**: Every 0→1 transition spends time setting up a deadline entity that might be torn down microseconds later on the 1→0 transition. For workloads where tasks are short-lived or do tiny amounts of work between wakeup and sleep, the overhead of managing the dl_server can exceed the actual useful scheduling work being done.

This is not a crash or data corruption bug — it is a performance design flaw. The system remains functionally correct; CFS tasks are still scheduled and receive their fair share. But the overhead of the dl_server management mechanism degrades throughput and increases latency for an important class of workloads.

## Fix Summary

The fix replaces the aggressive (immediate) dl_server stop/start pattern with a lazy two-phase shutdown mechanism. The key changes are:

**1. New `dl_server_idle` flag and `dl_server_stopped()` function:** A new bit field `dl_server_idle` is added to `struct sched_dl_entity`. A new function `dl_server_stopped()` implements the lazy shutdown logic:
- If `dl_server_active == 0`: the server is already stopped, return `false` (no action needed). *(Note: this was later fixed in commit `4717432dfd99` to return the correct value — see the existing driver analysis for that follow-up bug.)*
- If `dl_server_idle == 1`: this is the second consecutive period with no CFS tasks, so actually call `dl_server_stop()` and return `true`.
- Otherwise: set `dl_server_idle = 1` as a "first strike" marker and return `false` — the server stays active for now.

**2. Reset idle flag on activity:** In `dl_server_update()`, which is called whenever a CFS task consumes runtime through the dl_server, `dl_se->dl_server_idle` is reset to 0. This means any CFS activity within a period cancels the pending lazy shutdown.

**3. Remove aggressive stop calls from fair.c:** Both `dl_server_stop()` calls in `dequeue_entities()` and `throttle_cfs_rq()` are completely removed. The dl_server is no longer stopped when CFS tasks depart — it simply remains active. The associated `rq_h_nr_queued` snapshot variables that were only used for the stop check are also removed.

**4. Lazy stop via pick path and timer:** `dl_server_stopped()` is called from two places:
- In `__pick_task_dl()`: when the dl_server is picked but has no CFS task to run. Instead of the old `if (dl_server_active(dl_se))` check, the code now calls `if (!dl_server_stopped(dl_se))` — if the server wasn't stopped (still in first idle period), it yields and updates as before; if it was stopped (second idle period), it skips.
- In `dl_server_timer()`: when the replenishment timer fires and there are no tasks, `dl_server_stopped()` is called after replenishing, which advances the two-phase shutdown state machine.

**5. Guard against redundant starts:** In `dl_server_start()`, an early return is added for `dl_se->dl_server_active` — if the server is already active (because it was never stopped due to the lazy mechanism), `dl_server_start()` becomes a no-op. This is the key optimization: since the server stays active through brief idle periods, subsequent `dl_server_start()` calls on the 0→1 transition find it already active and return immediately, eliminating the enqueue overhead entirely.

The net effect is that for workloads with rapid 0→1→0 transitions, the dl_server remains active continuously (never reaching the two-period idle threshold), making both `dl_server_start()` and `dl_server_stop()` effectively free. The default period is 1 second, so the server only shuts down after a CPU has had no CFS tasks for a full second — a reasonable threshold that real workloads rarely trigger.

## Triggering Conditions

The following conditions are needed to trigger the performance regression:

- **dl_server must be enabled**: The dl_server (fair server) must be active. After commit `5f6bd380c7bd` ("sched/rt: Remove default bandwidth control"), this is the default on all systems. On older kernels where the original fair server commit `557a6bfc662c` was present but not enabled by default, the bug technically existed but was not exercised.

- **Rapid 0→1→0 CFS task transitions on a CPU**: The workload must cause CPUs to frequently alternate between having zero CFS tasks and one or more CFS tasks. This happens naturally with request-response workloads where a task wakes up, does a small unit of work, and sleeps. The key metric is the frequency of these transitions — the higher the frequency, the worse the overhead.

- **Multiple CPUs**: While the bug can occur on a single CPU, the regression is most visible on multi-CPU systems where many CPUs independently experience the same oscillation pattern. The schbench benchmark uses 4 message groups (`-m 4`) with 128 threads (`-t 128`) spread across CPUs, creating many independent transition streams.

- **No special kernel configuration required**: The bug triggers with default kernel configuration as long as `CONFIG_SMP=y` (which enables the dl_server). No cgroup configuration, special scheduling classes, or topology setup is needed.

- **No timing precision requirement**: The bug is not a race condition — it reliably occurs whenever the transition frequency is high enough. The overhead is deterministic and proportional to the number of start/stop cycles.

- **Task affinity not critical**: The tasks do not need to be pinned to specific CPUs, though the regression is most pronounced when tasks naturally spread across CPUs (via the scheduler's load balancing) such that each CPU sees its own set of waking/sleeping tasks.

The regression can be observed with the `schbench` benchmark using the command: `schbench -L -m 4 -M auto -t 128 -n 0 -r 60`, which creates a high context-switch-rate workload that hammers the 0→1, 1→0 transitions. However, any workload with similar characteristics (rapid task wakeup/sleep cycles) will trigger the same overhead.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP by demonstrating the functional difference in dl_server behavior between the buggy and fixed kernels. While kSTEP cannot measure wall-clock performance overhead directly, it can observe the internal state changes that cause the overhead — specifically, whether `dl_server_active` oscillates on every CFS task transition (buggy) or stays stable (fixed).

### Setup

1. **QEMU configuration**: 2 CPUs minimum. No special topology needed.

2. **Task creation**: Create a single CFS task pinned to CPU 1 (not CPU 0, which is reserved for the driver).

3. **Internal symbol access**: Use `KSYM_IMPORT` or direct access to `cpu_rq(1)->fair_server` to read the `dl_server_active`, `dl_server_idle`, and other dl_server state fields.

### Test Sequence

The driver should implement the following steps:

**Phase 1: Establish baseline — dl_server starts**
1. Create a CFS task `p` and pin it to CPU 1 using `kstep_task_pin(p, 1, 1)`.
2. Wake up the task with `kstep_task_wakeup(p)`. This triggers `enqueue_task_fair()` → `dl_server_start()`.
3. Advance several ticks with `kstep_tick_repeat(5)` to let the dl_server settle.
4. Read `cpu_rq(1)->fair_server.dl_server_active` — it should be 1 on both buggy and fixed kernels.
5. Log the initial state.

**Phase 2: Block the task — observe dl_server behavior on 1→0 transition**
1. Block the task with `kstep_task_block(p)`. This triggers `dequeue_entities()`.
2. Advance a few ticks with `kstep_tick_repeat(3)`.
3. Read `cpu_rq(1)->fair_server.dl_server_active`:
   - **Buggy kernel**: Should be **0** — `dl_server_stop()` was called immediately in `dequeue_entities()`.
   - **Fixed kernel**: Should be **1** — the dl_server remains active; `dl_server_idle` may be set to 1 via `dl_server_stopped()` in `__pick_task_dl()`.
4. On the fixed kernel, also read `cpu_rq(1)->fair_server.dl_server_idle` — it should be 1 (first idle period marker).

**Phase 3: Wake the task again — observe dl_server behavior on 0→1 transition**
1. Wake the task with `kstep_task_wakeup(p)`. This triggers `enqueue_task_fair()` → `dl_server_start()`.
2. On the **buggy kernel**, `dl_server_start()` must do full enqueue work because `dl_server_active` was 0.
3. On the **fixed kernel**, `dl_server_start()` returns early because `dl_server_active` is already 1 (early return guard added by the fix).
4. Advance ticks and read `dl_server_active` — should be 1 on both kernels.
5. On the fixed kernel, read `dl_server_idle` — should be 0 (reset by `dl_server_update()` when the CFS task runs through the server).

**Phase 4: Rapid cycling — demonstrate repeated behavior**
1. Perform multiple block/wakeup cycles (e.g., 10 iterations), each time reading `dl_server_active` after blocking:
   - **Buggy kernel**: `dl_server_active` should toggle to 0 after each block.
   - **Fixed kernel**: `dl_server_active` should remain 1 throughout all cycles.
2. Count the number of times `dl_server_active` was observed as 0. On the buggy kernel this count should equal the number of block operations; on the fixed kernel it should be 0.

**Phase 5 (optional): Verify lazy shutdown actually works on fixed kernel**
1. Block the task so the runqueue has 0 CFS tasks.
2. Advance enough ticks to span a full dl_server period (the default period is 1000ms = 1 second). Since the default tick interval is typically 1ms or 4ms, this requires `kstep_tick_repeat(250)` to `kstep_tick_repeat(1000)` depending on HZ.
3. Wait for the dl_server timer to fire (the replenishment timer). After one period, `dl_server_stopped()` sets `dl_server_idle = 1`. After the second period, it actually calls `dl_server_stop()`.
4. Read `dl_server_active` — it should now be 0 even on the fixed kernel, confirming the lazy shutdown eventually does stop the server.

### Detection Criteria

Use `kstep_pass()` / `kstep_fail()` based on:
- **Buggy kernel detection**: After blocking the CFS task and ticking a few times, if `fair_server.dl_server_active == 0`, the bug is present (aggressive stop).
- **Fixed kernel detection**: After blocking the CFS task and ticking a few times, if `fair_server.dl_server_active == 1`, the fix is applied (lazy stop).

The pass/fail criteria should be:
- On the **buggy kernel**: `kstep_fail("dl_server_active dropped to 0 immediately after last CFS task dequeued — aggressive stop overhead present")`.
- On the **fixed kernel**: `kstep_pass("dl_server_active remained 1 after CFS task dequeued — lazy shutdown working correctly")`.

### kSTEP API Requirements

This driver requires access to `struct rq` internals via `cpu_rq()` and reading `rq->fair_server` fields (`dl_server_active`, `dl_server_idle`). These are accessible through kSTEP's internal access to `kernel/sched/sched.h` internals. The `dl_server_idle` field was added by the fix commit itself, so on the buggy kernel it does not exist — the driver should use `#if LINUX_VERSION_CODE` guards to conditionally check `dl_server_idle` only on kernels where the fix is applied, or simply rely on `dl_server_active` as the primary detection signal (which exists on both versions).

No new kSTEP API additions are needed. The existing `kstep_task_create()`, `kstep_task_pin()`, `kstep_task_wakeup()`, `kstep_task_block()`, `kstep_tick_repeat()`, `cpu_rq()`, and `kstep_pass()`/`kstep_fail()` are sufficient.

### Expected Output

On the buggy kernel, after blocking the CFS task:
```
fair_server.dl_server_active = 0  (stopped immediately)
```

On the fixed kernel, after blocking the CFS task:
```
fair_server.dl_server_active = 1  (still active, lazy shutdown pending)
fair_server.dl_server_idle = 1    (first idle period marker set)
```
