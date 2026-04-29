# Deadline: DL Server Time Accounting Double-Update and Inactive Server Accounting

**Commit:** `c7f7e9c73178e0e342486fd31e7f363ef60e3f83`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.13-rc3
**Buggy since:** v6.12-rc1 (introduced by `a110a81c52a9` "sched/deadline: Deferrable dl server")

## Bug Description

The Linux kernel's deadline server (dl_server) mechanism uses a per-CPU `fair_server` entity of type `struct sched_dl_entity` to guarantee that CFS (fair) tasks receive CPU time even when higher-priority RT tasks are present. The fair_server is configured with a CBS (Constant Bandwidth Server) budget (default 50ms runtime per 1000ms period). When the deferrable dl_server feature was introduced in commit `a110a81c52a9`, it added a "defer" mode where the dl_server does not immediately proxy CFS tasks but instead defers its activation until CFS tasks would otherwise be starved by RT tasks.

In this deferrable mode, CFS task execution time must be accounted against the fair_server in two distinct scenarios: (1) when the fair_server directly proxies a CFS task (the task's `p->dl_server` pointer is set to `&rq->fair_server`), and (2) when the fair_server is active but deferred, and a CFS task runs after being picked through the normal fair class pick path (the task's `p->dl_server` is NULL). The buggy code attempted to handle both cases with two separate `dl_server_update()` calls placed in different locations, but the logic for deciding when to call each was flawed.

The first call was in `update_curr_task()`, which checked `if (p->dl_server)` and called `dl_server_update(p->dl_server, delta_exec)`. This function is called from both `update_curr()` (CFS path) and `update_curr_common()` (RT/DL class path). The second call was in `update_curr()`, which checked `if (p->dl_server != &rq->fair_server)` and called `dl_server_update(&rq->fair_server, delta_exec)`. The intent of the second call was to account time against the fair_server for CFS tasks not proxied through it, but the condition and placement of both calls led to incorrect behavior when the dl_server was inactive (stopped/dequeued) or in certain race conditions.

This commit is the second of a two-patch series. The companion commit `b53127db1dbf` ("sched/dlserver: Fix dlserver double enqueue") introduces a `dl_server_active` flag to track the active state of the dl_server. This commit uses that flag to consolidate and fix the time accounting logic in `fair.c`.

## Root Cause

The root cause is twofold: the `update_curr_task()` function called `dl_server_update()` without checking whether the dl_server was actually active (started and enqueued), and the condition in `update_curr()` used the wrong predicate (`p->dl_server != &rq->fair_server`) instead of checking the fair_server's active state.

**Problem 1: Unconditional dl_server_update in update_curr_task()**

In the buggy code, `update_curr_task()` contained:
```c
static inline void update_curr_task(struct task_struct *p, s64 delta_exec)
{
    trace_sched_stat_runtime(p, delta_exec);
    account_group_exec_runtime(p, delta_exec);
    cgroup_account_cputime(p, delta_exec);
    if (p->dl_server)
        dl_server_update(p->dl_server, delta_exec);
}
```

The check `if (p->dl_server)` only verifies that the task has a dl_server pointer set, not that the dl_server entity is actually active. The companion commit's description explains how the dl_server can get dequeued (stopped) during a pick operation due to the delayed dequeue feature: when `__pick_next_task` → `pick_task_dl` → `server_pick_task` → `pick_task_fair` → `pick_next_entity` encounters a `sched_delayed` entity, `dequeue_entities` is called which triggers `dl_server_stop`. After this, the dl_server is dequeued but the task's `p->dl_server` pointer still references it. The subsequent `update_curr_task()` call then invokes `dl_server_update()` on the stopped dl_server, feeding delta_exec into `update_curr_dl_se()`. This function manipulates the dl_server's `runtime`, `dl_throttled`, and `dl_yielded` state, and may call `enqueue_dl_entity()` in its throttle path, potentially re-enqueuing an entity that was just dequeued — a double enqueue.

Furthermore, `update_curr_task()` is called from `update_curr_common()`, which is used by the RT and DL scheduling classes. If an RT task's `donor->dl_server` were ever non-NULL (which shouldn't normally happen for RT tasks but is not explicitly prevented), this path would also incorrectly update the dl_server.

**Problem 2: Wrong condition in update_curr()**

In `update_curr()`, the buggy code had:
```c
if (p->dl_server != &rq->fair_server)
    dl_server_update(&rq->fair_server, delta_exec);
```

This condition is semantically wrong. Its intent was: "if the task is NOT running on behalf of fair_server (i.e., it's running as a normal CFS task while fair_server is deferred), then account its time against the fair_server." But the condition `p->dl_server != &rq->fair_server` is true whenever `p->dl_server` is NULL (which is the common case for CFS tasks not proxied through the dl_server) AND when `p->dl_server` points to some other server. Critically, it does NOT check whether the fair_server is actually active. If the fair_server is stopped (e.g., because all CFS tasks have been dequeued and a new one just arrived but the fair_server hasn't been started yet, or due to the delayed dequeue race), this code still calls `dl_server_update(&rq->fair_server, delta_exec)`, corrupting the stopped server's accounting state.

The combination of these two problems means that `dl_server_update()` could be called on a stopped/dequeued fair_server through either path, leading to corrupted runtime accounting, incorrect throttle/yield states, and ultimately double enqueue of the dl_server entity on the DL runqueue.

## Consequence

The primary consequence is **double enqueue of the dl_server entity on the DL runqueue**, which causes a kernel BUG/WARNING. When `dl_server_update()` is called on a stopped dl_server that has corrupted `dl_throttled` and `dl_yielded` flags (set as a side effect of the dequeue-during-pick scenario described in the companion commit), the `update_curr_dl_se()` function enters its throttle path and calls `enqueue_dl_entity()`. If the dl_server was already re-enqueued by a concurrent `dl_server_start()` (Case 2 from the companion patch), this results in the entity being on the DL runqueue twice, corrupting the RB-tree structure.

The double enqueue manifests as a kernel warning or crash in the DL scheduler's red-black tree operations (`__enqueue_dl_entity()`), as inserting an already-present entity corrupts the tree's invariants. This was observed by Marcel Ziswiler on ROCK 5B (ARM64) hardware and by Ilya Maximets, both of whom provided Tested-by tags.

Secondary consequences include:
- **Incorrect fair_server runtime accounting**: calling `dl_server_update()` on a stopped server decrements its `runtime` field via `update_curr_dl_se()`, even though no server time is actually being consumed. This can cause the server's runtime to go negative or become desynchronized from its period, leading to premature throttling or excessive runtime grants in subsequent periods.
- **Incorrect throttle/yield state**: the stopped server may have its `dl_throttled` or `dl_yielded` flags set or cleared at inappropriate times, confusing the replenishment timer logic and potentially preventing the server from being properly restarted.
- **RT task starvation potential**: if the fair_server's accounting is corrupted such that it never properly throttles, CFS tasks may receive unbounded execution time through the dl_server, starving RT tasks.

The race condition in Case 2 (described in the companion commit) involves two CPUs and is particularly insidious: one CPU is in `schedule()` → `pick_next_task_fair()` → `sched_balance_newidle()` which releases the runqueue lock, while another CPU takes the lock and performs `try_to_wake_up()` → `activate_task()` → `dl_server_start()` (first enqueue) → `wakeup_preempt()` → `update_curr()` → `update_curr_task()` → `dl_server_update()` → `enqueue_dl_entity()` (second enqueue).

## Fix Summary

The fix makes two changes to `kernel/sched/fair.c`:

**Change 1: Remove dl_server_update from update_curr_task()**

The lines `if (p->dl_server) dl_server_update(p->dl_server, delta_exec);` are removed from `update_curr_task()`. This eliminates the unconditional per-task dl_server update that could operate on a stopped/dequeued server. After this change, `update_curr_task()` only handles generic task accounting (tracing, cgroup accounting) without touching dl_server state:

```c
static inline void update_curr_task(struct task_struct *p, s64 delta_exec)
{
    trace_sched_stat_runtime(p, delta_exec);
    account_group_exec_runtime(p, delta_exec);
    cgroup_account_cputime(p, delta_exec);
}
```

**Change 2: Use dl_server_active() check in update_curr()**

The condition `if (p->dl_server != &rq->fair_server)` is replaced with `if (dl_server_active(&rq->fair_server))`:

```c
if (dl_server_active(&rq->fair_server))
    dl_server_update(&rq->fair_server, delta_exec);
```

The `dl_server_active()` function (introduced by the companion commit in `sched.h`) returns the value of `dl_se->dl_server_active`, a flag that is set to 1 in `dl_server_start()` and cleared to 0 in `dl_server_stop()`. This ensures `dl_server_update()` is only called when the fair_server is genuinely active (started, on the DL runqueue, and tracking CFS execution).

This single check in `update_curr()` correctly handles both accounting scenarios:
1. **Task running on behalf of fair_server** (`p->dl_server == &rq->fair_server`): the fair_server is active, so `dl_server_active()` returns true, and `dl_server_update()` properly accounts the proxied execution time.
2. **Task running independently while fair_server is deferred** (`p->dl_server == NULL`): the fair_server is still active (it was started and remains on the DL runqueue in deferred mode), so `dl_server_active()` returns true, and the execution time is correctly accounted against the fair_server so it can avoid running this period.
3. **Fair_server is stopped/dequeued**: `dl_server_active()` returns false, and `dl_server_update()` is not called, preventing any corruption of the stopped server's state.

The fix is correct and complete because it consolidates two separate, error-prone accounting paths into a single, clean check that relies on an authoritative flag (`dl_server_active`) rather than indirect pointer comparisons.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

- **Kernel version**: v6.12-rc1 through v6.13-rc2 (contains `a110a81c52a9` "Deferrable dl server" but lacks both `b53127db1dbf` and `c7f7e9c73178`).
- **Kernel configuration**: `CONFIG_SMP=y` (needed for Case 2 race condition), and the default dl_server/fair_server configuration (enabled by default with 50ms/1000ms runtime/period).
- **CPU count**: At least 2 CPUs for the race condition scenario (Case 2). Case 1 (delayed dequeue during pick) can occur on a single CPU.
- **DELAY_DEQUEUE feature**: The `DELAY_DEQUEUE` scheduler feature must be enabled (it is enabled by default in kernels containing the EEVDF scheduler changes). This is required for Case 1.

**Case 1 — Delayed dequeue during dl_server pick (single CPU):**
1. At least one CFS task must be on the runqueue so the fair_server is active.
2. A CFS task must have entered a sleep state (e.g., `TASK_INTERRUPTIBLE`) but with delayed dequeue, its sched_entity remains on the CFS runqueue with `sched_delayed = 1`.
3. During `__pick_next_task` → `pick_task_dl` → `server_pick_task` → `pick_task_fair` → `pick_next_entity`, the delayed entity is encountered. If it's the only eligible entity, `dequeue_entities` is called, which triggers `dl_server_stop` (because `cfs_rq->nr_running` drops to 0).
4. After `dl_server_stop`, `server_pick_task` returns NULL. The buggy `__pick_task_dl` then calls `update_curr_dl_se(rq, dl_se, 0)` which sets `dl_yielded = 1` and enters the throttle path, potentially calling `enqueue_dl_entity()` on the just-stopped server.
5. In subsequent scheduling, when a new CFS task is enqueued, `dl_server_start()` is called, which also calls `enqueue_dl_entity()`, causing a double enqueue.
6. Meanwhile, if a CFS task was still running during step 4, `update_curr()` → `update_curr_task()` calls `dl_server_update(p->dl_server, delta_exec)` on the stopped fair_server (because `p->dl_server` still points to it), further corrupting the accounting.

**Case 2 — Race between task dequeue and remote wakeup (multi-CPU):**
1. CPU A is in `schedule()`: it calls `deactivate_task()` which triggers `dl_server_stop()`, then enters `pick_next_task_fair()` → `sched_balance_newidle()` which releases `rq_lock(this_rq)`.
2. While CPU A's runqueue lock is released, CPU B executes `try_to_wake_up()` for a task targeted at CPU A: it takes CPU A's `rq_lock()`, calls `activate_task()` → `dl_server_start()` (first enqueue of fair_server), then calls `wakeup_preempt()` → `check_preempt_wakeup_fair()` → `update_curr()` → `update_curr_task()`.
3. At this point, the current task on CPU A still has `p->dl_server` set from before the `dl_server_stop`. The buggy `update_curr_task()` calls `dl_server_update(p->dl_server, delta_exec)` → `update_curr_dl_se()` → `enqueue_dl_entity()` (second enqueue), causing a double enqueue.

The bug probability depends on workload characteristics: it requires CFS tasks going to sleep and waking up frequently (triggering delayed dequeue and dl_server start/stop cycles). Systems with mixed RT and CFS workloads and frequent task migrations are most susceptible.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by simulating the Case 1 scenario (delayed dequeue during dl_server pick). The key is to create conditions where a CFS task is delayed-dequeued, causing `dl_server_stop` during the pick path, and then observe the incorrect time accounting against the stopped fair_server.

**Step 1: QEMU Configuration**
Configure QEMU with at least 2 CPUs. CPU 0 is reserved for the driver; the bug will be triggered on CPU 1.

**Step 2: Verify Kernel Feature State**
Before proceeding, verify that `DELAY_DEQUEUE` is enabled by reading the sched_features file via:
```c
kstep_sysctl_write("/sys/kernel/debug/sched/features", "DELAY_DEQUEUE");
```
Or simply rely on it being the default. Also verify the fair_server is initialized by checking `cpu_rq(1)->fair_server.dl_runtime > 0`.

**Step 3: Create CFS Tasks**
Create 2 CFS tasks and pin them to CPU 1:
```c
struct task_struct *t1 = kstep_task_create();
struct task_struct *t2 = kstep_task_create();
kstep_task_pin(t1, 1, 2);  // pin to CPU 1
kstep_task_pin(t2, 1, 2);  // pin to CPU 1
```
This ensures the fair_server on CPU 1 is activated (dl_server_start is called when the first CFS task is enqueued on an idle runqueue).

**Step 4: Advance Time to Establish DL Server State**
Run several ticks to let the fair_server establish its CBS parameters and enter a stable state:
```c
kstep_tick_repeat(10);
```

**Step 5: Block One Task to Trigger Delayed Dequeue**
Block task t2 so it enters `TASK_INTERRUPTIBLE` state. With `DELAY_DEQUEUE` enabled, the task's sched_entity remains on the CFS runqueue with `sched_delayed = 1` instead of being immediately dequeued:
```c
kstep_task_block(t2);
```

**Step 6: Block the Remaining Task**
Now block task t1 as well. When t1 is blocked and the scheduler runs pick_next_task:
- `pick_task_dl` picks the fair_server
- `server_pick_task` calls `pick_task_fair`
- `pick_next_entity` encounters t2's delayed entity and dequeues it via `dequeue_entities`
- This causes `cfs_rq->nr_running` to drop to 0, triggering `dl_server_stop`
- The buggy `__pick_task_dl` then calls `update_curr_dl_se(rq, dl_se, 0)` on the just-stopped server
```c
kstep_task_block(t1);
kstep_tick();  // trigger the pick path
```

**Step 7: Observe the Bug**
Use `KSYM_IMPORT` to access internal symbols and read the fair_server state on CPU 1:
```c
struct rq *rq1 = cpu_rq(1);
struct sched_dl_entity *fs = &rq1->fair_server;
```

On the **buggy kernel**, check for:
- `fs->dl_yielded == 1` (set by the buggy __pick_task_dl path after dl_server_stop)
- `fs->dl_throttled == 1` (set by update_curr_dl_se's throttle path on the stopped server)
- The fair_server being enqueued on the DL runqueue despite being "stopped" (check `rq1->dl.dl_nr_running` — it should be 0 if the server is stopped, but may be > 0 due to double enqueue)
- The fair_server's `runtime` being decremented even though it's stopped

Additionally, observe what happens when t1 or t2 is woken up again:
```c
kstep_task_wakeup(t1);
kstep_tick();
```
On the buggy kernel, `dl_server_start()` will call `enqueue_dl_entity()`, but if the server is already on the DL runqueue (due to the earlier buggy re-enqueue in the throttle path), this causes a double enqueue, potentially triggering a kernel WARNING or corrupting the DL RB-tree.

**Step 8: Detect and Report**
```c
// After blocking both tasks and ticking
if (fs->dl_yielded || fs->dl_throttled) {
    kstep_fail("fair_server has dl_yielded=%d dl_throttled=%d after stop",
               fs->dl_yielded, fs->dl_throttled);
} else {
    kstep_pass("fair_server correctly inactive after stop");
}

// After waking t1
if (rq1->dl.dl_nr_running > 1) {
    kstep_fail("dl_nr_running=%d suggests double enqueue", rq1->dl.dl_nr_running);
}
```

On the **buggy kernel** (before both fix commits), we expect to see `dl_yielded` and/or `dl_throttled` set on the stopped fair_server, and potentially a double enqueue when the server is restarted. On the **fixed kernel**, the fair_server state should be clean after `dl_server_stop()`, and `dl_server_update()` should not be called when the server is inactive.

**Step 9: Alternative Detection via update_curr Path**
To specifically target the `update_curr()` accounting bug (this commit's fix), keep one task running while the other is blocked:
```c
// Reset: wake both tasks
kstep_task_wakeup(t1);
kstep_task_wakeup(t2);
kstep_tick_repeat(5);

// Record fair_server runtime
s64 runtime_before = fs->runtime;

// Block t2 (will be delayed-dequeued)
kstep_task_block(t2);
kstep_tick_repeat(3);  // t1 continues running, update_curr is called

// On buggy kernel: update_curr_task calls dl_server_update(p->dl_server, delta)
// even if p->dl_server points to a server that got stopped during pick
// On fixed kernel: only dl_server_active check in update_curr is used
s64 runtime_after = fs->runtime;

// The fair_server should have consumed runtime (it's active because t1 is running)
// But on buggy kernel, the accounting may be double-counted (once through
// update_curr_task and once through update_curr's own check)
```

**kSTEP Extensions Needed:**
- None fundamental. The existing `kstep_task_block()` and `kstep_task_wakeup()` APIs should suffice to trigger delayed dequeue behavior.
- Access to `cpu_rq(cpu)->fair_server` fields (runtime, dl_throttled, dl_yielded, dl_server_active) via `internal.h` is already available.
- Access to `cpu_rq(cpu)->dl.dl_nr_running` for detecting double enqueue is available via internal headers.
- A callback on `on_tick_begin` or `on_tick_end` could be used to observe the dl_server state at precise scheduling points.

**Expected Results:**
- **Buggy kernel**: fair_server state corruption (dl_yielded/dl_throttled set on stopped server), potential double enqueue visible via `dl_nr_running`, kernel WARNING from DL RB-tree operations.
- **Fixed kernel**: fair_server cleanly stopped, no corrupted flags, `dl_server_update` only called when `dl_server_active()` returns true, `dl_nr_running` correct.
