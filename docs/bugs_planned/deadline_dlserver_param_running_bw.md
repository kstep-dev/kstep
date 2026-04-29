# Deadline: dl_server Parameter Change Breaks running_bw Tracking

**Commit:** `bb4700adc3abec34c0a38b64f66258e4e233fc16`
**Affected files:** `kernel/sched/debug.c`
**Fixed in:** v6.17-rc4
**Buggy since:** v6.17-rc1 (commit `cccb45d7c4295` "sched/deadline: Less agressive dl_server handling")

## Bug Description

The Linux kernel's deadline scheduler provides a "dl_server" mechanism — a deadline-scheduled entity (`rq->fair_server`) that guarantees CFS tasks a minimum share of CPU bandwidth even in the presence of RT tasks. The dl_server is configured with runtime/period parameters (default 50ms/1000ms) and is exposed via debugfs at `/sys/kernel/debug/sched/fair_server/cpu<N>/runtime` and `/sys/kernel/debug/sched/fair_server/cpu<N>/period`.

Commit `cccb45d7c4295` ("sched/deadline: Less agressive dl_server handling") introduced a lazy stopping optimization to reduce dl_server overhead. Previously, the dl_server was immediately stopped via `dl_server_stop()` whenever all CFS tasks dequeued from a runqueue (in `dequeue_entities()` and `throttle_cfs_rq()`). This caused performance problems for workloads that rapidly oscillate between 0 and 1 CFS tasks, since each transition incurred the cost of stopping and restarting the dl_server. The optimization delayed the actual stop by introducing a two-phase idle mechanism: after CFS tasks depart, the first period marks `dl_server_idle = 1` via `dl_server_stopped()`, and only after a second consecutive period without tasks does the server actually stop. This means the deadline entity remains enqueued (and contributes to `running_bw`) for up to one additional period after all CFS tasks leave.

However, this lazy stopping created a window during which a request to change the dl_server's runtime/period parameters via the debugfs interface could corrupt per-runqueue `running_bw` accounting. The `sched_fair_server_write()` function in `debug.c` conditionally calls `dl_server_stop()` only when `rq->cfs.h_nr_queued > 0`, but with the lazy mechanism, the dl_server can still be active (enqueued, contributing to `running_bw`) even when `h_nr_queued == 0`. The parameter change updates `dl_se->dl_bw` to a new value while `running_bw` still reflects the old value, creating an accounting mismatch that leads to `running_bw` underflow or corruption when the server eventually stops.

## Root Cause

The root cause is a conditional guard in `sched_fair_server_write()` (in `kernel/sched/debug.c`, line 379 in the buggy version) that was not updated to account for the lazy dl_server stopping behavior introduced by `cccb45d7c4295`.

The buggy code in `sched_fair_server_write()`:
```c
if (rq->cfs.h_nr_queued) {
    update_rq_clock(rq);
    dl_server_stop(&rq->fair_server);
}

retval = dl_server_apply_params(&rq->fair_server, runtime, period, 0);
```

Before `cccb45d7c4295`, when `h_nr_queued == 0`, the dl_server was guaranteed to already be stopped (since `dequeue_entities()` and `throttle_cfs_rq()` would call `dl_server_stop()` immediately). So the condition `if (rq->cfs.h_nr_queued)` was a valid optimization: if no CFS tasks, the server is already stopped, no need to stop again.

After `cccb45d7c4295`, this invariant is broken. The dl_server can be active (`dl_server_active == 1`) with bandwidth contribution to `running_bw` even when `h_nr_queued == 0`. The lazy mechanism means:
- `dl_server_stop()` calls were removed from `dequeue_entities()` in `fair.c`
- `dl_server_stop()` calls were removed from `throttle_cfs_rq()` in `fair.c`
- Instead, `dl_server_stopped()` in `deadline.c` implements a two-phase idle detection: first call sets `dl_server_idle = 1` (returns false), second call actually invokes `dl_server_stop()` (returns true)

When `sched_fair_server_write()` skips `dl_server_stop()` due to `h_nr_queued == 0`, and then calls `dl_server_apply_params()`, the following happens inside `dl_server_apply_params()` (line 1675 of the buggy `deadline.c`):

```c
int dl_server_apply_params(struct sched_dl_entity *dl_se, u64 runtime, u64 period, bool init)
{
    u64 old_bw = init ? 0 : to_ratio(dl_se->dl_period, dl_se->dl_runtime);
    u64 new_bw = to_ratio(period, runtime);
    ...
    /* init is false for parameter changes */
    dl_rq_change_utilization(rq, dl_se, new_bw);
    ...
    dl_se->dl_bw = to_ratio(dl_se->dl_period, dl_se->dl_runtime);
    ...
}
```

The `dl_rq_change_utilization()` function (line 297) updates `this_bw` but does NOT properly update `running_bw`:

```c
static void dl_rq_change_utilization(struct rq *rq, struct sched_dl_entity *dl_se, u64 new_bw)
{
    if (dl_se->dl_non_contending) {
        sub_running_bw(dl_se, &rq->dl);   /* only if non-contending */
        dl_se->dl_non_contending = 0;
        ...
    }
    __sub_rq_bw(dl_se->dl_bw, &rq->dl);   /* this_bw -= old */
    __add_rq_bw(new_bw, &rq->dl);          /* this_bw += new */
}
```

Since the dl_server was never stopped (never went through `dequeue_dl_entity(DEQUEUE_SLEEP)` → `task_non_contending()`), the `dl_non_contending` flag is 0. So the `if (dl_se->dl_non_contending)` branch is NOT taken, and `running_bw` is left unchanged. After `dl_server_apply_params()`:
- `this_bw` is correctly updated: old bandwidth subtracted, new bandwidth added
- `running_bw` is WRONG: still reflects the old `dl_bw` value
- `dl_se->dl_bw` is updated to the new value

This creates a fundamental accounting mismatch between `running_bw` and `dl_se->dl_bw`.

## Consequence

The consequence is corruption of the per-runqueue `running_bw` field in `struct dl_rq`, which tracks the total bandwidth of all active (contending) deadline entities. This corruption manifests in two ways depending on whether the new bandwidth is larger or smaller than the old:

**Case 1: Increased runtime (new_bw > old_bw):** When the dl_server eventually stops (via `dl_server_stop()` → `dequeue_dl_entity(DEQUEUE_SLEEP)` → `task_non_contending()` → `sub_running_bw(dl_se, dl_rq)`), it subtracts `dl_se->dl_bw` (the NEW, larger value) from `running_bw` (which still holds the OLD, smaller value). This causes an unsigned integer underflow in `__sub_running_bw()`:
```c
void __sub_running_bw(u64 dl_bw, struct dl_rq *dl_rq)
{
    u64 old = dl_rq->running_bw;
    dl_rq->running_bw -= dl_bw;
    WARN_ON_ONCE(dl_rq->running_bw > old); /* underflow! */
    if (dl_rq->running_bw > old)
        dl_rq->running_bw = 0;             /* clamped to 0 */
}
```
The `WARN_ON_ONCE` fires, producing a kernel warning with a stack trace. The value is clamped to 0, but the corruption has already occurred.

**Case 2: Decreased runtime (new_bw < old_bw):** When the dl_server stops, `sub_running_bw` subtracts the new (smaller) value, leaving a positive residual in `running_bw` that should be 0. This residual will trigger `WARN_ON_ONCE(dl_rq->running_bw > dl_rq->this_bw)` in `__sub_rq_bw()` the next time `this_bw` is decremented, since the leftover `running_bw` may exceed the adjusted `this_bw`.

In both cases, the corrupted `running_bw` affects GRUB (Greedy Reclamation of Unused Bandwidth) reclaiming decisions and cpufreq utilization calculations, potentially leading to incorrect CPU frequency scaling and bandwidth allocation for other deadline tasks on the same runqueue.

## Fix Summary

The fix removes the conditional guard `if (rq->cfs.h_nr_queued)` around the `dl_server_stop()` call in `sched_fair_server_write()`, making the stop unconditional:

```c
/* Fixed code: */
update_rq_clock(rq);
dl_server_stop(&rq->fair_server);

retval = dl_server_apply_params(&rq->fair_server, runtime, period, 0);
```

This ensures that `dl_server_stop()` is always called before `dl_server_apply_params()`, regardless of whether there are CFS tasks queued. The `dl_server_stop()` function itself is safe to call when the server is already inactive — it checks `if (!dl_server(dl_se) || !dl_server_active(dl_se)) return;` and returns immediately if the server is not active. When the server IS active (as in the lazy-stop case), it properly dequeues the deadline entity via `dequeue_dl_entity(dl_se, DEQUEUE_SLEEP)`, which calls `task_non_contending()` to correctly handle `running_bw` before the parameters are changed.

After `dl_server_apply_params()`, the existing code restarts the server with `dl_server_start()` only if `rq->cfs.h_nr_queued > 0`. This is correct: if there are CFS tasks, the server should be re-enqueued with the new bandwidth; if not, the server remains stopped (matching the desired state). The fix is minimal (2 lines added, 4 lines removed) and precisely targets the problematic window.

## Triggering Conditions

The bug requires the following precise conditions:

1. **Kernel version**: The kernel must include commit `cccb45d7c4295` ("sched/deadline: Less agressive dl_server handling") which was merged in v6.17-rc1. This commit introduced the lazy dl_server stopping mechanism that opens the vulnerability window. The bug was fixed in v6.17-rc4 by commit `bb4700adc3abec34c0a38b64f66258e4e233fc16`.

2. **CONFIG_SCHED_DEBUG must be enabled**: The `sched_fair_server_write()` function is only compiled and the `/sys/kernel/debug/sched/fair_server/` debugfs entries are only created when `CONFIG_SCHED_DEBUG` is enabled. This is the default for most distribution kernels and all development kernels.

3. **dl_server must be active on the target CPU**: The dl_server starts when the first CFS task enqueues on a CPU. It must have been started at least once so that `dl_server_active == 1` and `running_bw` includes its bandwidth contribution.

4. **All CFS tasks must be removed from the CPU**: `rq->cfs.h_nr_queued` must be 0 on the target CPU. This can happen when all CFS tasks on the CPU block, migrate away, or are dequeued. With the lazy mechanism, the dl_server remains active (`dl_server_active == 1`) for up to one full period (default 1 second) after this transition.

5. **Parameter change must occur during the lazy-stop window**: A write to `/sys/kernel/debug/sched/fair_server/cpu<N>/runtime` or `/sys/kernel/debug/sched/fair_server/cpu<N>/period` must occur while `h_nr_queued == 0` AND `dl_server_active == 1`. This window lasts from when the last CFS task dequeues until the second call to `dl_server_stopped()` actually stops the server (up to ~1 second with default period).

6. **The new parameter value must differ from the old**: If the same value is written, the `if (runtime == value) break;` / `if (value == period) break;` early exits prevent `dl_server_apply_params()` from being called, so the bug is not triggered.

The bug is deterministic given the above conditions — there is no race condition beyond the timing window. The probability of hitting it in production depends on whether administrative tooling changes fair_server parameters during periods of low CFS activity on a specific CPU.

## Reproduce Strategy (kSTEP)

The reproduce strategy uses kSTEP to create the exact conditions described above. The driver will:

**Step 1: Configure QEMU with at least 2 CPUs.** CPU 0 is reserved for the driver; CPU 1 will be the target CPU where the bug is triggered.

**Step 2: Create and wake a CFS task on CPU 1.** Use `kstep_task_create()` to create a CFS task, then `kstep_task_pin(p, 1, 1)` to pin it to CPU 1, and `kstep_task_wakeup(p)` to enqueue it. This triggers `enqueue_task_fair()` → `dl_server_start()` on CPU 1, which activates the fair server, enqueues the deadline entity, and adds `dl_bw` to `running_bw`.

**Step 3: Advance time to let the dl_server stabilize.** Call `kstep_tick_repeat(n)` with enough ticks to allow the dl_server to go through at least one replenishment cycle. This ensures the dl_server is fully active with `dl_server_active == 1`, `dl_server_idle == 0`, and `running_bw` reflects the default bandwidth (50ms/1000ms).

**Step 4: Record baseline `running_bw`.** Using `KSYM_IMPORT` and `cpu_rq(1)`, read and record `cpu_rq(1)->dl.running_bw` and `cpu_rq(1)->fair_server.dl_bw`. Verify `running_bw > 0` and `fair_server.dl_server_active == 1`. Also record `cpu_rq(1)->dl.this_bw`.

**Step 5: Block the CFS task.** Call `kstep_task_block(p)` to dequeue the CFS task from CPU 1. This makes `rq->cfs.h_nr_queued == 0`. With the lazy stopping mechanism (cccb45d7c4295), `dl_server_stop()` is NOT called, so `dl_server_active` remains 1 and `running_bw` is unchanged.

**Step 6: Verify the lazy-stop window is open.** Check that `cpu_rq(1)->fair_server.dl_server_active == 1` and `cpu_rq(1)->cfs.h_nr_queued == 0`. This confirms the dl_server is still active despite no CFS tasks.

**Step 7: Write new fair_server parameters.** Use `kstep_write("/sys/kernel/debug/sched/fair_server/cpu1/runtime", "100000000", 9)` to change the runtime from the default 50ms (50000000 ns) to 100ms (100000000 ns). This triggers the buggy `sched_fair_server_write()` code path. On the buggy kernel, the `if (rq->cfs.h_nr_queued)` check fails (it's 0), so `dl_server_stop()` is skipped. `dl_server_apply_params()` is called, updating `dl_se->dl_bw` to the new bandwidth ratio while `running_bw` still reflects the old ratio. On the fixed kernel, `dl_server_stop()` is called unconditionally before `dl_server_apply_params()`, properly handling `running_bw`.

**Step 8: Detect the bug by checking `dl_server_active`.** Read `cpu_rq(1)->fair_server.dl_server_active`:
- On the **buggy kernel**: `dl_server_active == 1` — the server was NOT stopped before the parameter change, because `h_nr_queued == 0` caused the stop to be skipped. The server is still "active" with stale `running_bw`.
- On the **fixed kernel**: `dl_server_active == 0` — the server was unconditionally stopped before the parameter change. Since `h_nr_queued == 0`, `dl_server_start()` was not called afterward, so the server remains stopped.

**Step 9: Additional verification — check `running_bw` consistency.** After the debugfs write:
- On the **buggy kernel**: `running_bw` still equals the OLD `dl_bw` ratio (for 50ms/1000ms), but `dl_se->dl_bw` is now the NEW ratio (for 100ms/1000ms). This is the accounting mismatch. Log both values.
- On the **fixed kernel**: `running_bw` was properly decremented by `dl_server_stop()` → `task_non_contending()`. If `zerolag_time < 0`, `running_bw` is 0 immediately. Otherwise, `dl_non_contending` is set and the inactive timer will eventually clear it.

**Step 10: Pass/fail criteria.**
- Call `kstep_fail()` if `dl_server_active == 1` after the parameter write with `h_nr_queued == 0` (buggy behavior).
- Call `kstep_pass()` if `dl_server_active == 0` after the parameter write (fixed behavior, server was properly stopped).
- Additionally log `running_bw`, `this_bw`, `dl_se->dl_bw`, and `dl_non_contending` for diagnostic purposes.

**Callbacks:** No special callbacks (`on_tick_begin`, etc.) are needed. The driver's `run()` function performs the entire sequence synchronously.

**kSTEP compatibility notes:** This bug is fully reproducible with existing kSTEP APIs. `kstep_write()` (which uses `filp_open` + `kernel_write`) can write to debugfs files. Access to internal scheduler state is available through `cpu_rq()` from `internal.h`. The `KSYM_IMPORT` mechanism is not needed for this driver since all required state is accessible through the `rq` structure. The driver needs at least 2 CPUs configured in QEMU.
