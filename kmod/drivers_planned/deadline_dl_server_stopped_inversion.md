# Deadline: dl_server_stopped() returns inverted result for inactive server

**Commit:** `4717432dfd99bbd015b6782adca216c6f9340038`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.17-rc4
**Buggy since:** v6.17-rc1 (introduced by commit `cccb45d7c429` "sched/deadline: Less agressive dl_server handling")

## Bug Description

The `dl_server_stopped()` function, introduced in commit `cccb45d7c429`, contains a logic inversion bug in its very first check. The function is intended to implement a lazy shutdown mechanism for the dl_server (deadline server) — instead of immediately stopping the dl_server every time the CFS runqueue becomes empty (which causes performance overhead on workloads with frequent 0→1, 1→0 task transitions), the new design keeps the dl_server alive for one full period after the last CFS task departs, only stopping it if it remains idle for an entire period.

The bug is that when `dl_se->dl_server_active` is 0 (meaning the server is already inactive/stopped), `dl_server_stopped()` returns `false` instead of `true`. This means callers that check "is the server stopped?" get the wrong answer: they are told the server is NOT stopped when it actually IS stopped, and vice versa.

This affects two call sites: (1) `dl_server_timer()`, where it is called after replenishing a dl_entity that has no tasks — the inverted return value does not cause a direct crash here since the return value is ignored, but it prevents the lazy shutdown state machine from functioning correctly. (2) `__pick_task_dl()`, where the return value gates whether `dl_yielded` is set and `update_curr_dl_se()` is called — with the inverted logic, this code path is entered when the server is already stopped (doing unnecessary work) and skipped when the server is still active (missing the yield/update step).

The patch author, Huacai Chen, found the bug because after commit `cccb45d7c429`, a "sched: DL replenish lagged too much" warning message appeared after every boot, indicating the dl_server's timing state was getting corrupted due to this logic error.

## Root Cause

The root cause is a simple but consequential boolean inversion in `dl_server_stopped()`. The function was written as:

```c
static bool dl_server_stopped(struct sched_dl_entity *dl_se)
{
    if (!dl_se->dl_server_active)
        return false;     // BUG: should be true
    ...
}
```

The intended semantics of `dl_server_stopped()` are: "return true if the dl_server is stopped (or just became stopped), false if it is still running." When `dl_server_active` is 0, the server is definitively stopped — the function should return `true`. Returning `false` here tells callers the server is still active when it is not.

The full intended logic of `dl_server_stopped()` implements a two-phase lazy shutdown. The `dl_server_idle` flag acts as a "first strike" marker:

1. If `dl_server_active == 0`: The server is already stopped. Return `true` (the fix).
2. If `dl_server_idle == 1`: This is the second consecutive period with no CFS tasks. Call `dl_server_stop()` to actually deactivate the server. Return `true`.
3. Otherwise (first idle period): Set `dl_server_idle = 1` as a warning marker. Return `false` — the server stays active for now.

The `dl_server_idle` flag is reset to 0 in `dl_server_update()` whenever the server actually executes work (i.e., a CFS task consumes runtime through the dl_server). This means if a CFS task arrives before the next period expires, `dl_server_idle` gets cleared, and the server continues running without interruption.

With the bug, when the server is already stopped (`dl_server_active == 0`), `dl_server_stopped()` returns `false`, which causes `__pick_task_dl()` to enter the `!dl_server_stopped()` branch. This sets `dl_se->dl_yielded = 1` and calls `update_curr_dl_se(rq, dl_se, 0)` on an already-stopped server entity, which can corrupt the dl_server's timing state and lead to the "DL replenish lagged too much" warning.

## Consequence

The primary observable consequence is a "sched: DL replenish lagged too much" kernel warning message appearing after boot. This warning is emitted by `replenish_dl_entity()` when it detects that a deadline entity's timing parameters are inconsistent — specifically when the entity has lagged so far behind that normal replenishment cannot bring it up to date.

The bug causes the dl_server's yield and update paths to execute on an already-stopped server, corrupting its internal accounting (runtime, deadline, period tracking). This leads to the dl_server's timing state becoming inconsistent with wall-clock time, triggering the replenishment lag warning on subsequent timer firings.

Beyond the warning message, the inverted logic also prevents the lazy shutdown mechanism from working correctly. Instead of keeping the dl_server alive for one period to absorb rapid 0→1 transitions (which was the entire purpose of commit `cccb45d7c429`), the broken state machine means the dl_server may either stay alive indefinitely when it should have been shut down, or get improperly poked when already stopped. This defeats the performance optimization that the parent commit was designed to provide. While not a crash or security issue, it degrades scheduler correctness and generates spurious kernel warnings on every boot.

## Fix Summary

The fix is a one-character change: in `dl_server_stopped()`, the `return false` on line 1614 is changed to `return true`. This correctly reports that a server with `dl_server_active == 0` is indeed stopped.

```c
static bool dl_server_stopped(struct sched_dl_entity *dl_se)
{
    if (!dl_se->dl_server_active)
-       return false;
+       return true;
    ...
}
```

With this fix, the two call sites behave correctly: (1) In `__pick_task_dl()`, when `dl_server_stopped()` returns `true` for an already-inactive server, the `!dl_server_stopped()` condition is `false`, so the code skips the yield/update path and goes directly to `goto again` to re-pick, which is the correct behavior — there's no point yielding or updating a stopped server. (2) In `dl_server_timer()`, the return value is not used (it's a void context call), but the function now correctly advances the lazy shutdown state machine: calling `dl_server_stopped()` on an already-stopped server is a no-op that returns `true`, while calling it on an active server with no tasks either marks it as idle (first call) or stops it (second consecutive call).

This fix restores the intended lazy shutdown semantics: the dl_server survives one full idle period before being shut down, the "DL replenish lagged too much" warning goes away, and the performance optimization from commit `cccb45d7c429` works as designed.

## Triggering Conditions

The bug is triggered on every boot of a kernel containing commit `cccb45d7c429` (v6.17-rc1 through v6.17-rc3), because the dl_server is initialized and used by default for CFS bandwidth control. The specific conditions are:

- **Kernel version**: Any kernel with commit `cccb45d7c429` but without commit `4717432dfd99`. This is v6.17-rc1 through v6.17-rc3.
- **CPU count**: At least 1 CPU (the dl_server is per-CPU, initialized for every online CPU).
- **CFS tasks**: The bug triggers whenever a CFS runqueue transitions from having tasks to being empty. This happens routinely during normal boot and operation.
- **dl_server enabled**: The fair_server must be enabled (which is the default; runtime = 50ms, period = 1000ms).

The specific trigger sequence in `__pick_task_dl()` is:
1. The dl_server is picked as the next dl entity (it's on the dl runqueue).
2. `server_pick_task(dl_se)` returns NULL (no CFS tasks to run).
3. `dl_server_stopped(dl_se)` is called with `dl_server_active == 0` (server already stopped from a previous cycle).
4. BUG: Returns `false` instead of `true`.
5. Code enters the yield/update path on the stopped server, corrupting its state.

The bug is highly deterministic and will trigger on every boot, not just under specific workloads. The "DL replenish lagged too much" warning message is the telltale sign.

## Reproduce Strategy (kSTEP)

The core idea is to create a scenario where the dl_server goes through its lazy shutdown cycle and then gets picked again while already stopped, exercising the buggy `dl_server_stopped()` return value.

### Setup

1. **Topology**: 2 CPUs minimum. CPU 0 is reserved for the driver; the bug will be triggered on CPU 1.
2. **Kernel version guard**: `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,17,0)` — the buggy code only exists from v6.17-rc1 onward.

### Step-by-step plan

1. **Create a CFS task and pin it to CPU 1**:
   ```c
   struct task_struct *p = kstep_task_create();
   kstep_task_pin(p, 1, 1);
   ```
   This will cause `dl_server_start()` to be called on CPU 1's fair_server, setting `dl_server_active = 1`.

2. **Wake the task and let it run briefly**:
   ```c
   kstep_task_wakeup(p);
   kstep_tick_repeat(5);
   ```
   This ensures the dl_server is active and has executed some work, so `dl_server_idle` is reset to 0 via `dl_server_update()`.

3. **Block the task to make the CFS runqueue empty**:
   ```c
   kstep_task_block(p);
   ```
   With the lazy shutdown design, the dl_server remains active (not immediately stopped). On the buggy kernel, `dl_server_stopped()` will be called in `__pick_task_dl()` when the scheduler tries to pick from the dl_server and `server_pick_task()` returns NULL.

4. **Advance time through ticks to trigger the timer and pick paths**:
   ```c
   kstep_tick_repeat(100);  // enough ticks to cover at least one dl_server period (1 second)
   ```
   During these ticks, the scheduler will call `__pick_task_dl()` and encounter the dl_server with no tasks. With the bug, `dl_server_stopped()` returns `false` when the server is actually stopped, causing the yield/update path to execute on a stopped server.

5. **Observe the bug via internal state inspection**:
   Using `KSYM_IMPORT` to access `cpu_rq(1)->fair_server`, check the dl_server's state:
   ```c
   struct rq *rq1 = cpu_rq(1);
   struct sched_dl_entity *dl_se = &rq1->fair_server;
   ```

   After the task blocks and sufficient ticks pass:
   - **On buggy kernel**: `dl_se->dl_server_active` may be 0 but `dl_se->dl_yielded` is 1 (set by the erroneously entered yield path). Also check for `dl_se->dl_throttled` state inconsistencies. The "DL replenish lagged too much" message may appear in `dmesg`/kernel log.
   - **On fixed kernel**: When `dl_server_active == 0`, `dl_server_stopped()` returns `true`, the yield path is skipped, and `dl_yielded` remains 0. No warning messages.

6. **Detection criteria**:
   - **Primary check**: Use an `on_tick_begin` callback on CPU 1 to monitor `fair_server.dl_server_active` and `fair_server.dl_yielded`. If `dl_server_active == 0 && dl_yielded == 1`, the bug has been triggered (the yield was applied to a stopped server).
   - **Secondary check**: Monitor kernel log output for "DL replenish lagged too much" using `printk` interception or simply checking `dmesg` after the test.
   - **Pass condition**: `dl_yielded` is never set to 1 while `dl_server_active` is 0. No "DL replenish lagged" warnings.
   - **Fail condition**: `dl_yielded` is observed as 1 while `dl_server_active` is 0, or "DL replenish lagged" warning is seen.

7. **Repeat the cycle for robustness**: Wake the task again, let it run, block it again, and observe. Repeat 3-5 times to confirm deterministic behavior:
   ```c
   for (int i = 0; i < 5; i++) {
       kstep_task_wakeup(p);
       kstep_tick_repeat(5);
       kstep_task_block(p);
       kstep_tick_repeat(200);
       // check dl_server state
   }
   ```

### Key internal symbols to import

- `cpu_rq` (already available via `internal.h`)
- Access `rq->fair_server` (a `struct sched_dl_entity` embedded in `struct rq`)
- Read fields: `dl_server_active`, `dl_server_idle`, `dl_yielded`, `dl_throttled`, `runtime`, `deadline`

### Expected behavior

- **Buggy kernel (cccb45d7c429~1 + cccb45d7c429)**: After the CFS task blocks and the dl_server period elapses, `dl_server_stopped()` returns `false` when `dl_server_active == 0`, causing `dl_yielded` to be set on a stopped server. The "DL replenish lagged too much" warning appears in the kernel log.
- **Fixed kernel (4717432dfd99)**: `dl_server_stopped()` correctly returns `true` when `dl_server_active == 0`. The yield path is skipped. No warnings, no state corruption.

### Notes on timing

The dl_server has a default period of 1000ms (1 second). To cover a full period in kSTEP ticks, use `kstep_tick_repeat()` with enough ticks to span at least 1 second of virtual time. The typical tick interval is `HZ`-dependent (usually 1ms or 4ms), so 250-1000 ticks should be sufficient. Alternatively, use `kstep_sleep()` or `kstep_sleep_until()` to wait for the timer to fire.
