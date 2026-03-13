# Core: Uncookied-to-Cookied Task Misses Core Tree Enqueue

**Commit:** `91caa5ae242465c3ab9fd473e50170faa7e944f4`
**Affected files:** kernel/sched/core_sched.c
**Fixed in:** v6.0-rc1
**Buggy since:** v5.14-rc1 (introduced by `6e33cad0af49` "sched: Trivial core scheduling cookie management")

## Bug Description

Core scheduling is a Linux kernel feature designed to mitigate SMT (Simultaneous Multi-Threading) side-channel attacks by ensuring that only mutually trusted tasks run concurrently on the same physical core. Tasks are grouped by "cookies" — each cookie value represents a trust domain, and the scheduler ensures that at any given moment, all tasks running on SMT siblings of a physical core share the same cookie (or one of the siblings is forced idle).

The bug occurs in the `sched_core_update_cookie()` function in `kernel/sched/core_sched.c`. When a task that previously had no cookie (`core_cookie == 0`) is assigned a new cookie, it is not inserted into the per-runqueue core scheduling rb-tree (`rq->core_tree`). This happens because the function uses the variable `enqueued` — set from `sched_core_enqueued(p)` — to gate both the dequeue-from-tree and the re-enqueue-into-tree operations. Since a task with no cookie is never placed in the core tree in the first place (`sched_core_enqueue()` returns early when `p->core_cookie == 0`), `sched_core_enqueued(p)` returns `false`, and the newly-cookied task is never added to the tree.

The consequence is that the core scheduling algorithm cannot see the newly-cookied task when making scheduling decisions. The task effectively remains invisible to core scheduling until it goes through a full dequeue-enqueue cycle (e.g., by being migrated, sleeping and waking up, or changing priority). During this window, the scheduler treats the task as uncookied and may force its SMT sibling idle unnecessarily, even though the sibling is running a task with the same cookie. This results in significant unnecessary forced-idle time, degrading throughput by up to 30% in the affected scenario described in the commit message.

## Root Cause

The root cause is a logic error in `sched_core_update_cookie()` that uses a single boolean to control two operations that have different correctness requirements.

In the buggy code:

```c
static unsigned long sched_core_update_cookie(struct task_struct *p,
                                              unsigned long cookie)
{
    unsigned long old_cookie;
    struct rq_flags rf;
    struct rq *rq;
    bool enqueued;

    rq = task_rq_lock(p, &rf);
    SCHED_WARN_ON((p->core_cookie || cookie) && !sched_core_enabled(rq));

    enqueued = sched_core_enqueued(p);   // (A)
    if (enqueued)
        sched_core_dequeue(rq, p, DEQUEUE_SAVE);  // (B)

    old_cookie = p->core_cookie;
    p->core_cookie = cookie;             // (C)

    if (enqueued)                        // (D) BUG: reuses stale condition
        sched_core_enqueue(rq, p);

    if (task_running(rq, p))
        resched_curr(rq);

    task_rq_unlock(rq, p, &rf);
    return old_cookie;
}
```

The function `sched_core_enqueued(p)` (line A) checks `!RB_EMPTY_NODE(&p->core_node)` — i.e., whether the task is currently in the core tree. The function `sched_core_enqueue()` (called from `enqueue_task()` in `core.c`) only inserts a task into the core tree if `p->core_cookie != 0`:

```c
void sched_core_enqueue(struct rq *rq, struct task_struct *p)
{
    rq->core->core_task_seq++;
    if (!p->core_cookie)
        return;
    rb_add(&p->core_node, &rq->core_tree, rb_sched_core_less);
}
```

Therefore, a task with `core_cookie == 0` will never be in the core tree, and `sched_core_enqueued(p)` will always return `false` for it. There are four cases to consider for `sched_core_update_cookie()`:

1. **no-cookie → cookie (the bug):** `enqueued = false`. Dequeue: skipped (correct — nothing to remove). Cookie is set to the new value. Enqueue: skipped at line (D) because `enqueued` is `false`. **BUG**: the task now has a cookie and is on the runqueue, but is NOT in the core tree.

2. **cookie → different cookie:** `enqueued = true` (task was in tree with old cookie). Dequeue: removes from tree. Cookie updated. Enqueue: re-inserts with new cookie. Correct.

3. **cookie → no-cookie:** `enqueued = true`. Dequeue: removes from tree. Cookie set to 0. Enqueue: `sched_core_enqueue()` returns early because `p->core_cookie == 0`. The `enqueued` check at (D) passes, but `sched_core_enqueue()` itself has an early return for `!p->core_cookie`, so this is functionally correct (task stays out of tree) — though it does a spurious `core_task_seq++`.

4. **no-cookie → no-cookie:** `enqueued = false`. Nothing happens. Correct.

The fundamental error is that the decision to enqueue after the cookie update should depend on the **new** cookie and the task's runqueue state, not on whether the task was previously in the core tree. The old `enqueued` boolean conflates "was previously in the tree" with "should now be in the tree."

## Consequence

The observable impact is unnecessary forced-idle scheduling on SMT cores. When a task is invisible to the core tree (missing from `rq->core_tree`), the core scheduling algorithm in `__pick_next_task()` cannot match it with tasks running on SMT siblings. Specifically, the function `sched_core_find()` walks the core tree to find a compatible task (one with a matching cookie) for each SMT sibling. If a task with a valid cookie is not in the tree, it will not be found, and the scheduler may decide to force-idle one of the siblings because it believes no compatible task is available.

The commit author provides a concrete reproduction scenario demonstrating the impact:

- CPU x and CPU y are SMT siblings.
- Task a runs on CPU x, tasks b and c run on CPU y, all without sleeping.
- Cookie α is assigned to tasks a and b (same trust domain). Cookie β is assigned to task c (different trust domain).
- Since task a gets its cookie while already running (no-cookie → cookie transition), it is not inserted into the core tree.
- When the scheduler on the core evaluates compatibility, it cannot find task a in the core tree, so it may force CPU x idle when task c is scheduled on CPU y — even though task b (with the same cookie as task a) is also runnable on CPU y.
- Sampling `core_forceidle_sum` from `/proc/PID/sched` for task a shows approximately 30% forced-idle time during the sampling period. This should be near zero since task a and task b have the same cookie and should always be scheduled concurrently.

After migrating the tasks to a different pair of SMT siblings (which triggers a dequeue-enqueue cycle that correctly inserts them into the core tree on the new CPUs), `core_forceidle_sum` drops to nearly zero, confirming that the bug is in the initial cookie assignment path.

This bug has real-world performance implications for workloads that use core scheduling for security isolation (e.g., cloud VMs using core scheduling to prevent cross-tenant SMT attacks). A 30% throughput loss due to unnecessary forced-idle is severe and directly undermines the purpose of core scheduling.

## Fix Summary

The fix replaces the single `enqueued` boolean with two separate, semantically correct conditions for dequeue and enqueue:

```c
static unsigned long sched_core_update_cookie(struct task_struct *p,
                                              unsigned long cookie)
{
    unsigned long old_cookie;
    struct rq_flags rf;
    struct rq *rq;
    // NOTE: 'bool enqueued' is removed

    rq = task_rq_lock(p, &rf);
    SCHED_WARN_ON((p->core_cookie || cookie) && !sched_core_enabled(rq));

    if (sched_core_enqueued(p))                    // Dequeue: check live state
        sched_core_dequeue(rq, p, DEQUEUE_SAVE);

    old_cookie = p->core_cookie;
    p->core_cookie = cookie;

    /*
     * Consider the cases: !prev_cookie and !cookie.
     */
    if (cookie && task_on_rq_queued(p))            // Enqueue: check NEW cookie + rq state
        sched_core_enqueue(rq, p);

    if (task_running(rq, p))
        resched_curr(rq);

    task_rq_unlock(rq, p, &rf);
    return old_cookie;
}
```

The dequeue check uses `sched_core_enqueued(p)` directly (equivalent to the old code, just without the intermediate variable). The critical change is in the enqueue condition: instead of `if (enqueued)`, it now uses `if (cookie && task_on_rq_queued(p))`. This correctly handles all four cases:

1. **no-cookie → cookie:** `cookie` is non-zero, and `task_on_rq_queued(p)` is true (task is on the runqueue) → enqueue into core tree. **Bug fixed.**
2. **cookie → different cookie:** `cookie` is non-zero, task is on rq → enqueue. Correct.
3. **cookie → no-cookie:** `cookie` is 0 → skip enqueue. Correct (and avoids the spurious `core_task_seq++` from the old code).
4. **no-cookie → no-cookie:** `cookie` is 0 → skip enqueue. Correct.

The additional `task_on_rq_queued(p)` check handles the edge case where the task is not on any runqueue at the time of the cookie update (e.g., it is in a sleeping state). In that case, there is no runqueue tree to insert into, and the task will be properly inserted when it is next woken up and enqueued via `enqueue_task()` → `sched_core_enqueue()`.

The fix also adds a comment `/* Consider the cases: !prev_cookie and !cookie. */` to document that the new condition intentionally handles the no-cookie-to-cookie and cookie-to-no-cookie transitions.

## Triggering Conditions

The following conditions must all hold simultaneously to trigger this bug:

1. **CONFIG_SCHED_CORE=y** must be enabled in the kernel configuration. Core scheduling must be active at runtime, meaning `sched_core_enabled(rq)` returns true. This requires at least one task in the system to have a non-zero `core_cookie`, which activates the `__sched_core_enabled` static key.

2. **SMT topology** is required. Core scheduling operates across SMT siblings within a physical core. The kernel checks `static_branch_likely(&sched_smt_present)` early in `sched_core_share_pid()` and returns `-ENODEV` if SMT is not present. The QEMU must be configured with SMT siblings (e.g., 2 threads per core).

3. **A task must be running (on the runqueue) when it transitions from no-cookie to cookie.** This is the specific code path that triggers the bug: the task must have `core_cookie == 0` and `on_rq == TASK_ON_RQ_QUEUED` at the time `sched_core_update_cookie()` is called with a new non-zero cookie. If the task is sleeping/blocked when the cookie is set, it won't hit this code path (and will get correctly enqueued into the core tree when it next wakes up).

4. **The cookie is set via `prctl(PR_SCHED_CORE, PR_SCHED_CORE_SHARE_TO, ...)` or an equivalent path** that calls `__sched_core_set()` → `sched_core_update_cookie()`. The cookie must be a valid non-zero cookie (allocated via `sched_core_alloc_cookie()` from a `PR_SCHED_CORE_CREATE` call).

5. **At least 2 CPUs** configured as SMT siblings. The driver CPU (CPU 0) must be separate from the SMT pair used for testing, so at least 3 CPUs total are needed (CPU 0 for the driver, CPUs 1-2 as an SMT pair).

6. **The bug is 100% reproducible** given the above conditions. It is not a race condition — it is a deterministic logic error. Every time an uncookied task that is currently on the runqueue gets assigned a cookie, it will fail to be inserted into the core tree. No special timing is needed.

## Reproduce Strategy (kSTEP)

This bug is reproducible with kSTEP using minor extensions (KSYM_IMPORT for core scheduling internal functions). The strategy mirrors the scenario described in the commit message: set up SMT siblings, create tasks, assign cookies to already-running tasks, and verify whether the tasks are correctly inserted into the core tree.

**Step 1: QEMU and topology configuration.** Configure QEMU with at least 4 CPUs. In the driver's `setup()` function, use `kstep_topo_init()` and `kstep_topo_set_smt()` to create SMT pairs: CPUs 1-2 as one pair, CPUs 3-4 as another (CPU 0 reserved for the driver). Apply with `kstep_topo_apply()`. This ensures `sched_smt_present` is set and `sched_core_enabled()` can return true on these CPUs.

**Step 2: Import core scheduling symbols.** Use `KSYM_IMPORT` to bring in the required internal functions and variables:
- `KSYM_IMPORT(sched_core_get)` — to enable core scheduling globally by incrementing the reference count and activating the `__sched_core_enabled` static key.
- `KSYM_IMPORT(sched_core_put)` — for cleanup.
- Access to `sched_core_enqueue()` and `sched_core_dequeue()` — these are non-static and declared in `kernel/sched/sched.h`, which is included via kSTEP's `internal.h`. They can be called directly.
- Direct access to `p->core_cookie`, `p->core_node`, `rq->core_tree`, and `rq->core->core_task_seq` — all accessible through `internal.h` which includes `kernel/sched/sched.h`.

**Step 3: Enable core scheduling.** In the driver's `run()` function, call `KSYM_sched_core_get()` to enable core scheduling globally. This is the first step to ensure `sched_core_enabled(rq)` returns true.

**Step 4: Create and place tasks.** Create three CFS tasks:
- Task A: `kstep_task_create()`, pinned to CPU 1 with `kstep_task_pin(a, 1, 1)`.
- Task B: `kstep_task_create()`, pinned to CPU 2 with `kstep_task_pin(b, 2, 2)`.
- Task C: `kstep_task_create()`, pinned to CPU 2 with `kstep_task_pin(c, 2, 2)`.

Wake all three tasks so they are on the runqueue and running (or runnable): `kstep_task_wakeup(a)`, `kstep_task_wakeup(b)`, `kstep_task_wakeup(c)`. Run several ticks with `kstep_tick_repeat(10)` to let them settle on their CPUs.

**Step 5: Allocate cookies and assign to running tasks.** Allocate two `sched_core_cookie` structs via `kmalloc()`, set `refcount_set(&ck->refcnt, 1)` on each. The cookie value is the address cast to `unsigned long`. For cookie α (shared by tasks A and B):
- Lock the rq for each task using `task_rq_lock()` (from `sched.h`).
- Verify `task_on_rq_queued(p)` is true (the task is on the runqueue).
- Record whether `sched_core_enqueued(p)` is true (should be false, since cookie was 0).
- Set `p->core_cookie = cookie_alpha`.
- Call `sched_core_enqueue(rq, p)` — or more precisely, to reproduce the bug, do NOT manually call this. Instead, the buggy kernel will skip it due to the logic error, and we detect the bug by checking if the task ended up in the core tree after the cookie update.
- Unlock.

Actually, the most accurate reproduction is to call `sched_core_update_cookie()` itself. Since it is `static`, it cannot be imported directly. Instead, we can replicate its logic in the driver: (1) `task_rq_lock()`, (2) dequeue from core tree if enqueued, (3) set cookie, (4) enqueue into core tree — but using the **buggy** condition. However, this would mean we are implementing the bug ourselves, not triggering it in the kernel.

A better approach: use `__sched_core_set()`. This function is also static, but we can use `KSYM_IMPORT_TYPED` with a function pointer type to import it via `kstep_ksym_lookup()`. Alternatively, since `sched_core_share_pid()` is the prctl handler and is a global (non-static) symbol, we can import it and call it with the appropriate arguments. But `sched_core_share_pid()` expects to operate on the `current` task or look up tasks by PID.

The simplest approach: since kSTEP tasks have valid PIDs, we can use `KSYM_IMPORT_TYPED` to import `sched_core_share_pid` and call it with `PR_SCHED_CORE_CREATE` on the current (driver) task first (to allocate a cookie), then `PR_SCHED_CORE_SHARE_TO` targeting our test tasks by PID. This exercises the exact buggy code path (`sched_core_update_cookie()`) as called from userspace prctl.

**Step 6: Verify core tree state.** After assigning cookies:
- For Task A (which was running on CPU 1 with no cookie when it got assigned cookie α):
  - Check `!RB_EMPTY_NODE(&a->core_node)` — this should be `true` (task should be in core tree), but on the **buggy kernel**, it will be `false`.
  - Also check `a->core_cookie != 0` — should be `true` on both buggy and fixed.

The pass/fail logic:
```c
if (a->core_cookie && task_on_rq_queued(a) && RB_EMPTY_NODE(&a->core_node)) {
    kstep_fail("task has cookie and is on rq but NOT in core_tree");
} else if (a->core_cookie && task_on_rq_queued(a) && !RB_EMPTY_NODE(&a->core_node)) {
    kstep_pass("task correctly in core_tree after cookie assignment");
} else {
    kstep_fail("unexpected state: cookie=%lu on_rq=%d",
               a->core_cookie, task_on_rq_queued(a));
}
```

**Step 7: Alternative detection via forced-idle observation.** For a more behavioral (less internal-state) detection, after assigning cookies:
- Run many ticks with `kstep_tick_repeat(100)` to let the scheduler make core scheduling decisions.
- On the buggy kernel, `rq->core->core_forceidle_count` or task A's `se.statistics.core_forceidle_sum` (if CONFIG_SCHEDSTATS is enabled) will show non-zero forced-idle time, since the scheduler doesn't know A and B share a cookie.
- On the fixed kernel, forced-idle should be minimal or zero since A and B have the same cookie.

This behavioral check is more robust but requires CONFIG_SCHEDSTATS=y and careful timing.

**Step 8: Cleanup.** After detection, clear cookies on all tasks (set `core_cookie` back to 0 with proper dequeue/enqueue), call `KSYM_sched_core_put()` to decrement the core scheduling reference count, and free the cookie structs.

**kSTEP changes needed:** The main extension required is importing `sched_core_share_pid` (or alternatively `sched_core_get`/`sched_core_put` plus direct cookie manipulation). These are minor KSYM_IMPORT usages, not fundamental framework changes. The pattern is already established in other planned drivers like `core_delayed_dequeue_core_sched.md`. No new framework APIs are needed.

**Expected behavior on buggy kernel:** After calling `sched_core_update_cookie()` on a task that was previously uncookied and is on the runqueue, `RB_EMPTY_NODE(&p->core_node)` returns `true` — the task is NOT in the core tree despite having a valid cookie and being on the rq. Forced-idle time accumulates unnecessarily.

**Expected behavior on fixed kernel:** After the same operation, `RB_EMPTY_NODE(&p->core_node)` returns `false` — the task IS correctly in the core tree. Forced-idle time is near zero for tasks sharing the same cookie.
