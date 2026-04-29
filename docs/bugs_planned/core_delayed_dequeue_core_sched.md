# Core: Delayed Dequeue vs Core Scheduling Double Dequeue

**Commit:** `c662e2b1e8cfc3b6329704dab06051f8c3ec2993`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.12-rc1
**Buggy since:** v6.12-rc1 (introduced by `152e11f6df29` "sched/fair: Implement delayed dequeue", which first appeared after v6.11-rc1)

## Bug Description

The delayed dequeue mechanism introduced in commit `152e11f6df29` ("sched/fair: Implement delayed dequeue") allows CFS tasks to remain on the runqueue after a sleep request if they are not yet eligible (according to EEVDF eligibility criteria). When a task calls `dequeue_task()` with `DEQUEUE_SLEEP`, the CFS fair class dequeue function (`dequeue_entity()`) may decide not to actually remove the entity from the CFS runqueue. Instead, it sets `se->sched_delayed = 1` and returns `false`, keeping the task on the runqueue to compete until it becomes eligible, at which point it will be fully dequeued.

The problem is that `dequeue_task()` in `kernel/sched/core.c` calls `sched_core_dequeue()` **before** calling the scheduling class's `dequeue_task()` method. When the CFS class "fails" the dequeue (delayed dequeue), the task remains on the runqueue, but `sched_core_dequeue()` has already removed the task from the core scheduling rb-tree (`rq->core_tree`) and incremented `rq->core->core_task_seq`. The task is now in an inconsistent state: it is still on the CFS runqueue (with `sched_delayed = 1`), but has been removed from the core scheduling data structures.

If a subsequent operation triggers a dequeue-enqueue cycle on this delayed task — such as a migration (via `deactivate_task()`/`activate_task()`), a priority change, or a cgroup move — `sched_core_dequeue()` and `sched_core_enqueue()` are called again. This leads to double accounting: spurious `core_task_seq` bumps and, critically, the re-insertion of a `sched_delayed` (conceptually sleeping) task into the `core_tree`, which corrupts core scheduling decisions. This bug is completely analogous to the uclamp double-decrement bug fixed in commit `dfa0a574cbc4` ("sched/uclamp: Handle delayed dequeue").

## Root Cause

The root cause lies in the ordering of operations within `dequeue_task()` and `enqueue_task()` in `kernel/sched/core.c`, and the lack of awareness of the `sched_delayed` state in the `sched_core_enqueue()` and `sched_core_dequeue()` functions.

In `dequeue_task()` (pre-fix), the call sequence is:
```c
inline bool dequeue_task(struct rq *rq, struct task_struct *p, int flags)
{
    if (sched_core_enabled(rq))
        sched_core_dequeue(rq, p, flags);   // (1) Remove from core_tree FIRST
    ...
    uclamp_rq_dec(rq, p);                   // (2) Decrement uclamp
    return p->sched_class->dequeue_task(...); // (3) CFS may "fail" → sched_delayed=1
}
```

Step (1) removes the task from `rq->core_tree` (if it had a cookie and was enqueued there), clears `p->core_node` via `RB_CLEAR_NODE()`, and increments `rq->core->core_task_seq`. Step (3) may then decide to delay the dequeue, setting `se->sched_delayed = 1` and returning `false`. The task remains on the CFS runqueue, but has already been removed from the core scheduling tree.

In `enqueue_task()`, the sequence is:
```c
void enqueue_task(struct rq *rq, struct task_struct *p, int flags)
{
    ...
    p->sched_class->enqueue_task(rq, p, flags); // CFS enqueue
    uclamp_rq_inc(rq, p);
    if (sched_core_enabled(rq))
        sched_core_enqueue(rq, p);               // Add to core_tree
}
```

Now consider the problematic scenario: a task with a non-zero `core_cookie` goes through a delayed dequeue (step 1 above removes it from `core_tree`, step 3 marks it `sched_delayed`). Later, a migration occurs via `deactivate_task()` → `dequeue_task()` → `sched_core_dequeue()`. Since the task's `core_node` was already cleared by `RB_CLEAR_NODE()`, `sched_core_enqueued(p)` returns false, so `rb_erase()` is skipped. However, `core_task_seq` is still spuriously incremented. Then `activate_task()` → `enqueue_task()` → `sched_core_enqueue()` fires: `core_task_seq` is incremented again, and `rb_add()` inserts the task back into the `core_tree` — even though the task is still `sched_delayed` and conceptually sleeping.

The function `sched_core_enqueue()` has no check for `sched_delayed`:
```c
void sched_core_enqueue(struct rq *rq, struct task_struct *p)
{
    rq->core->core_task_seq++;
    if (!p->core_cookie)
        return;
    rb_add(&p->core_node, &rq->core_tree, rb_sched_core_less);
}
```

It blindly adds the task to the tree regardless of whether it is truly runnable or merely lingering on the runqueue due to delayed dequeue.

## Consequence

The primary consequence is corruption of the core scheduling data structures. A `sched_delayed` task — one that is conceptually sleeping and waiting to be fully dequeued — gets re-inserted into the `core_tree` after a migration or similar dequeue-enqueue cycle. The core scheduling algorithm (`__schedule()` → core-wide scheduling path in `__pick_next_task()`) uses the `core_tree` to find compatible tasks to co-schedule on SMT siblings. A delayed task in the `core_tree` will be considered as a scheduling candidate, even though it cannot actually run. This can lead to incorrect core scheduling decisions: the scheduler may force an SMT sibling to go idle (forced idle) because it believes it needs to match cookies with a task that is actually sleeping.

Additionally, spurious `core_task_seq` increments cause unnecessary reschedule IPIs and re-evaluation of core scheduling state across all SMT siblings in the core. Each `core_task_seq` bump signals to the core scheduling logic that the set of runnable tasks has changed, triggering expensive cross-core synchronization even when nothing meaningful has changed.

In the worst case, this could lead to rb-tree corruption if the task's `core_node` is added to the tree while still linked from a previous insertion that was not properly cleaned up (though `RB_CLEAR_NODE()` in `sched_core_dequeue()` guards against this specific scenario for the initial dequeue). The more likely observable impact is degraded scheduling performance and incorrect forced-idle behavior on systems using core scheduling for security isolation (e.g., mitigating SMT side-channel attacks).

## Fix Summary

The fix adds an early-return guard at the top of both `sched_core_enqueue()` and `sched_core_dequeue()` that checks `p->se.sched_delayed`:

```c
void sched_core_enqueue(struct rq *rq, struct task_struct *p)
{
    if (p->se.sched_delayed)
        return;
    rq->core->core_task_seq++;
    ...
}

void sched_core_dequeue(struct rq *rq, struct task_struct *p, int flags)
{
    if (p->se.sched_delayed)
        return;
    rq->core->core_task_seq++;
    ...
}
```

When `sched_delayed` is set, the task has already been logically dequeued from the core scheduling perspective (during the initial `DEQUEUE_SLEEP` that triggered the delayed dequeue). Any subsequent dequeue-enqueue cycles on this task (from migration, reweight, etc.) should be invisible to core scheduling until the task is either fully dequeued (via `DEQUEUE_DELAYED`) or re-enqueued via a wakeup (via `ENQUEUE_DELAYED`). The `ENQUEUE_DELAYED` path in `enqueue_task_fair()` calls `requeue_delayed_entity()` which clears `sched_delayed`, after which subsequent core scheduling operations will proceed normally.

This fix is correct because it makes the core scheduling enqueue/dequeue functions idempotent with respect to delayed tasks. The initial dequeue already handled the core_tree removal; the eventual wakeup (which clears `sched_delayed` via `requeue_delayed_entity()`) will trigger a proper core scheduling enqueue. Any intermediate operations while the task is in the delayed state are correctly no-ops. This pattern is identical to the uclamp fix in commit `dfa0a574cbc4`.

## Triggering Conditions

The following conditions must all hold simultaneously to trigger this bug:

1. **CONFIG_SCHED_CORE=y** must be enabled in the kernel configuration, and core scheduling must be active at runtime (at least one task must have a non-zero `core_cookie`, which is set via `prctl(PR_SCHED_CORE, ...)`).

2. **SMT topology** is required. Core scheduling operates across SMT siblings within a physical core. Without SMT, `sched_core_enabled()` returns false and the affected code paths are never reached.

3. **DELAY_DEQUEUE sched feature** must be enabled (it is enabled by default: `SCHED_FEAT(DELAY_DEQUEUE, true)`). This is the mechanism that allows dequeue to "fail" and keep the task on the runqueue.

4. **A CFS task with a core_cookie** must go to sleep at a time when the EEVDF eligibility check in `dequeue_entity()` determines the entity is not eligible. This causes `dequeue_entity()` to set `sched_delayed = 1` and return `false`, keeping the task on the runqueue.

5. **While the task is in the `sched_delayed` state**, an operation that triggers a dequeue-enqueue cycle must occur. The most common triggers are:
   - **Migration**: load balancing moves the task to another CPU via `deactivate_task()`/`activate_task()`.
   - **Affinity change**: `set_cpus_allowed_ptr()` changes the task's allowed CPUs.
   - **Priority/nice change**: `set_user_nice()` or `sched_setattr()` triggers a requeue.
   - **Cgroup move**: moving the task to a different cgroup triggers dequeue/enqueue.

6. **At least 2 CPUs** are required (one for the driver on CPU 0, others for the workload with SMT pairing). The CPUs must be configured as SMT siblings.

The bug is relatively easy to trigger on systems with core scheduling enabled and moderate load, since delayed dequeue is common for CFS tasks that accumulate lag (negative eligibility), and migrations happen routinely via load balancing.

## Reproduce Strategy (kSTEP)

The key challenge for this reproduction is assigning core cookies to kSTEP-managed tasks without directly writing to the `p->core_cookie` field or manually calling `sched_core_enqueue()`/`sched_core_dequeue()`. The solution is to use `KSYM_IMPORT` to call `sched_core_share_pid()`, which is the kernel's own public API for assigning core cookies (the backend of `prctl(PR_SCHED_CORE, ...)`). This function is declared as `extern` in `include/linux/sched.h`, is non-static in `kernel/sched/core_sched.c`, and is available via kallsyms. Calling it handles all internal state management: it allocates the `sched_core_cookie` struct, calls `sched_core_get()` to enable core scheduling globally (activating the `__sched_core_enabled` static key), and calls `sched_core_update_cookie()` to properly dequeue the task from the core tree, set the cookie, and re-enqueue with correct locking. No direct writes to scheduler internals are needed. The driver only reads internal fields (`se.sched_delayed`, `core_node`, `core_task_seq`) for observation and pass/fail determination.

**Step 1: Kernel configuration and QEMU setup.** Build the kernel with `CONFIG_SCHED_CORE=y` to compile in core scheduling support. The `DELAY_DEQUEUE` sched feature is enabled by default and does not need explicit configuration. Configure QEMU with at least 5 CPUs (CPU 0 reserved for the kSTEP driver, CPUs 1–4 for the workload). Pass the boot parameter `idle=poll` to prevent CPUs from entering deep idle states, which improves tick-driven scheduling predictability. No `nosmt` parameter — SMT is required for core scheduling to function.

**Step 2: Topology setup.** Initialize the topology with `kstep_topo_init()`. Configure SMT pairs using `kstep_topo_set_smt()` so that CPUs 1 and 2 form one physical core and CPUs 3 and 4 form another. Apply with `kstep_topo_apply()`. This is mandatory: `sched_core_share_pid()` checks `static_branch_likely(&sched_smt_present)` at entry and returns `-ENODEV` if SMT is not present, and `sched_core_enabled(rq)` (which gates all the buggy code paths) returns false without SMT. After applying the topology, the kernel's scheduling domain hierarchy will recognize CPUs 1,2 and CPUs 3,4 as SMT sibling pairs.

**Step 3: Task creation and pinning.** Create three CFS tasks: `task_a` (the target that will undergo delayed dequeue and migration), `task_b` (a helper to maintain `nr_running > 1` on the same CPU, needed for EEVDF ineligibility), and `task_c` (a task on the SMT sibling to exercise core scheduling). Use `kstep_task_create()` for each. Pin `task_a` and `task_b` to CPU 1 with `kstep_task_pin(task_a, 1, 1)` and `kstep_task_pin(task_b, 1, 1)`. Pin `task_c` to CPU 2 with `kstep_task_pin(task_c, 2, 2)`. All three tasks should be in the default CFS scheduling class. Having `task_b` on the same CPU as `task_a` ensures the CFS runqueue on CPU 1 has `nr_running >= 2`, which is necessary for the EEVDF eligibility comparison that gates delayed dequeue (with only one entity, `avg_vruntime` equals the entity's own vruntime, so it is always eligible).

**Step 4: Assign core cookies via `sched_core_share_pid()`.** Import the function with `KSYM_IMPORT(sched_core_share_pid)`. To assign a unique core cookie to `task_a`, call `sched_core_share_pid(PR_SCHED_CORE_CREATE, task_pid_vnr(task_a), PIDTYPE_PID, 0)`. The `PR_SCHED_CORE_CREATE` command allocates a new `sched_core_cookie` via `sched_core_alloc_cookie()` (which internally calls `sched_core_get()` to activate core scheduling), then calls `__sched_core_set()` → `sched_core_update_cookie()` to set the cookie with proper rq locking and core-tree management. The `uaddr` parameter is unused for `CREATE` (only used for `GET`), so passing `0` is safe. The `ptrace_may_access()` check inside the function will succeed because the kSTEP driver runs in kernel context with init credentials (`CAP_SYS_PTRACE`). Optionally, also assign a cookie to `task_c` so that the core scheduling algorithm is actively matching cookies on the SMT pair: `sched_core_share_pid(PR_SCHED_CORE_CREATE, task_pid_vnr(task_c), PIDTYPE_PID, 0)`. After these calls, verify (read-only) that `task_a->core_cookie != 0` and that core scheduling is active by reading `sched_core_enabled(cpu_rq(1))`.

**Step 5: Build vruntime ineligibility for `task_a`.** The goal is to make `task_a`'s vruntime advance well ahead of the CFS runqueue's `avg_vruntime`, so that `entity_eligible(cfs_rq, &task_a->se)` returns false when `task_a` tries to sleep. First, pause `task_b` with `kstep_task_pause(task_b)` so only `task_a` runs on CPU 1. Run `kstep_tick_repeat(50)` or more to let `task_a` accumulate substantial vruntime. Then wake `task_b` with `kstep_task_wakeup(task_b)`. When `task_b` is placed back on the CFS runqueue, its vruntime is adjusted via the EEVDF placement logic to a favorable position (behind the average), which shifts `avg_vruntime` backward. Now `task_a`'s vruntime is ahead of the new `avg_vruntime`, making it ineligible. Run one or two more ticks with `kstep_tick_repeat(2)` to let `task_b`'s placement settle and to ensure both entities are properly accounted in the CFS runqueue's average.

**Step 6: Trigger delayed dequeue.** Call `kstep_task_block(task_a)` to make `task_a` go to sleep. This triggers the `__schedule()` → `dequeue_task()` → `dequeue_entity()` path with `DEQUEUE_SLEEP`. Inside `dequeue_entity()`, the condition `sched_feat(DELAY_DEQUEUE) && sleep && !entity_eligible(cfs_rq, se)` is checked. If `task_a` is ineligible (vruntime ahead of avg as arranged in Step 5), `set_delayed(se)` sets `se->sched_delayed = 1` and returns false — the entity stays on the CFS runqueue despite the sleep request. Crucially, in the buggy kernel, `dequeue_task()` in `core.c` has already called `sched_core_dequeue()` before the CFS class gets a chance to delay the dequeue. So `task_a` is removed from `rq->core_tree` and `core_task_seq` is incremented, but the task remains on the CFS runqueue with `sched_delayed = 1`. Run `kstep_tick()` to force the schedule point, then verify (read-only) that `task_a->se.sched_delayed == 1`. If it is not set (the task happened to be eligible at the exact moment of dequeue), retry: wake `task_a` with `kstep_task_wakeup(task_a)`, run more ticks to rebuild vruntime imbalance, and block again. Use `kstep_tick_until(fn)` with a check function that returns true when `task_a->se.sched_delayed == 1` to automate this retry loop.

**Step 7: Trigger dequeue-enqueue cycle via migration.** With `task_a` confirmed in the `sched_delayed` state, record the current `core_task_seq` value for comparison: `unsigned long seq_before = cpu_rq(1)->core->core_task_seq`. Now force a migration by changing `task_a`'s CPU affinity to CPU 3 (a different physical core's SMT sibling): `kstep_task_pin(task_a, 3, 3)`. This calls `set_cpus_allowed_ptr()` internally, which detects that `task_a` is currently queued on CPU 1 (it's on the CFS runqueue due to delayed dequeue) but CPU 1 is no longer in the allowed mask. The kernel migrates the task via `move_queued_task()` or `__migrate_task()`, which calls `deactivate_task()` on CPU 1 followed by `activate_task()` on CPU 3. Each of these calls goes through `dequeue_task()` and `enqueue_task()` in `core.c`, which call `sched_core_dequeue()` and `sched_core_enqueue()` respectively. On the **buggy kernel**, neither function checks `sched_delayed`, so: (a) the spurious `sched_core_dequeue()` increments `core_task_seq` again (though `rb_erase` is skipped since the node was already cleared), and (b) the subsequent `sched_core_enqueue()` increments `core_task_seq` and — critically — calls `rb_add()` to insert `task_a` into CPU 3's `core_tree`, despite `task_a` still being `sched_delayed`. Run `kstep_tick()` after the pin to let the migration complete.

**Step 8: Bug detection (read-only observation).** After the migration settles, perform the following read-only checks on `task_a` and the destination CPU's runqueue (`cpu_rq(3)`). The invariant being tested is: a task with `sched_delayed == 1` must NOT be present in the `core_tree`, because it has already been logically dequeued from the core scheduling perspective. Check `task_a->se.sched_delayed` and `RB_EMPTY_NODE(&task_a->core_node)`. On the **buggy kernel**, `sched_delayed` is `1` AND `RB_EMPTY_NODE` returns `false` (the node is in the tree) — this is the invariant violation. Report `kstep_fail("BUG: sched_delayed task re-inserted into core_tree after migration")`. On the **fixed kernel**, the early-return guards `if (p->se.sched_delayed) return;` in both `sched_core_enqueue()` and `sched_core_dequeue()` prevent any core-tree manipulation while the task is delayed. The task will NOT be in the `core_tree` (`RB_EMPTY_NODE` returns `true`). Report `kstep_pass("core_tree consistent: sched_delayed task not in core_tree")`.

```c
if (task_a->se.sched_delayed && !RB_EMPTY_NODE(&task_a->core_node)) {
    kstep_fail("sched_delayed task found in core_tree after migration");
} else {
    kstep_pass("core_tree consistent with sched_delayed state");
}
```

**Step 9: Additional `core_task_seq` validation.** As a secondary signal, compare `core_task_seq` values. Record `seq_before` (from Step 7) on the source core and `unsigned long seq_dest_before = cpu_rq(3)->core->core_task_seq` on the destination core before the migration. After migration, record `seq_dest_after = cpu_rq(3)->core->core_task_seq`. On the **buggy kernel**, the destination core's `core_task_seq` will have been incremented by at least 1 (from the spurious `sched_core_enqueue`), and the source core's by at least 1 (from the spurious `sched_core_dequeue`). On the **fixed kernel**, neither value changes because both operations are short-circuited by the `sched_delayed` guard. This provides a second observable signal: `if (seq_dest_after != seq_dest_before) kstep_fail("spurious core_task_seq bump on destination")`. Note that other scheduling activity on the cores could also bump `core_task_seq`, so this check is supplementary to the primary `core_node` check.

**Step 10: Internal access summary.** This strategy uses `KSYM_IMPORT(sched_core_share_pid)` to **call** a public kernel function — this is an API invocation, not a direct write to scheduler state. The function itself modifies `p->core_cookie`, `core_tree`, and `core_task_seq` internally through the kernel's own locking and accounting paths, which is the correct way to set core cookies. All other internal field accesses in this driver are **read-only** observations: reading `task_a->se.sched_delayed`, reading `RB_EMPTY_NODE(&task_a->core_node)`, and reading `rq->core->core_task_seq`. No scheduler fields are directly written by the driver. No changes to the kSTEP framework are needed — `KSYM_IMPORT` and direct struct reads from `sched.h` internals are existing capabilities.
