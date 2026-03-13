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

The reproduction requires enabling core scheduling, which is normally done via the `prctl(PR_SCHED_CORE, ...)` syscall. Since kSTEP operates as a kernel module without userspace process control, core scheduling must be enabled by directly calling internal kernel functions via `KSYM_IMPORT`. This is a minor extension: importing `sched_core_get()` to increment the core scheduling reference count and enable the `__sched_core_enabled` static key, then directly setting `p->core_cookie` on tasks while holding the appropriate rq lock.

**Step 1: Topology setup.** Configure QEMU with at least 4 CPUs. Set up SMT topology using `kstep_topo_set_smt()` with pairs like `{"1,2", "3,4"}` (keeping CPU 0 free for the driver). Apply the topology with `kstep_topo_apply()`. This is necessary for `sched_core_enabled(rq)` to return true.

**Step 2: Enable core scheduling.** Use `KSYM_IMPORT(sched_core_get)` to import and call `sched_core_get()`. This increments `sched_core_count` and enables the `__sched_core_enabled` static key, which activates core scheduling globally. This must be done before setting any cookies.

**Step 3: Create tasks with core cookies.** Create two or more CFS tasks with `kstep_task_create()`. Pin them to SMT siblings (e.g., CPUs 1 and 2) using `kstep_task_pin()`. To set `core_cookie`, use `KSYM_IMPORT` to access `sched_core_update_cookie` or, since it is static, directly manipulate the task struct: acquire the rq lock, call `sched_core_dequeue()` if the task is enqueued, set `p->core_cookie` to a non-zero value (e.g., a pointer to a kmalloc'd `sched_core_cookie` struct), then call `sched_core_enqueue()` if the task is on the rq. Alternatively, use `KSYM_IMPORT` on `__sched_core_set` which is a wrapper that handles cookie allocation and update. (Note: `__sched_core_set` is also static, so the most reliable approach is to allocate a `sched_core_cookie` struct, set its refcount, and assign its address as the cookie value directly.)

**Step 4: Trigger delayed dequeue.** Run ticks with `kstep_tick_repeat()` to let the tasks accumulate runtime and lag. Then use `kstep_task_block()` on one of the tasks with a cookie. If the task is not eligible at the moment of blocking, the CFS dequeue will set `sched_delayed = 1` and the task will remain on the runqueue. Verify by checking `se->sched_delayed` in the `on_tick_end` callback: `KSYM_IMPORT` the task struct and check `p->se.sched_delayed == 1`. If delayed dequeue doesn't trigger immediately, run more ticks first to build up vruntime and make the entity ineligible, or create additional competing tasks to shift `avg_vruntime`.

**Step 5: Trigger migration while delayed.** With the task in the `sched_delayed` state, change its CPU affinity using `kstep_task_pin(p, new_cpu_begin, new_cpu_end)` to force a migration. This triggers `deactivate_task()` on the old CPU and `activate_task()` on the new CPU, which is the dequeue-enqueue cycle that exposes the bug.

**Step 6: Detect the bug.** After the migration, check the `core_tree` state on the destination CPU. On the buggy kernel:
- `sched_core_enqueued(p)` will return `true` (the task was re-inserted into `core_tree` during migration enqueue), AND `p->se.sched_delayed` will still be `true`.
- This is the invariant violation: a `sched_delayed` task should never be in the `core_tree`.

Use `KSYM_IMPORT` to access `sched_core_enqueued` (inline function from `sched.h`, or check `!RB_EMPTY_NODE(&p->core_node)` directly). The pass/fail check is:
```c
if (p->se.sched_delayed && !RB_EMPTY_NODE(&p->core_node)) {
    kstep_fail("sched_delayed task found in core_tree after migration");
} else {
    kstep_pass("core_tree consistent with sched_delayed state");
}
```

On the **buggy kernel**, after the migration of a sched_delayed task with a core_cookie, the task will be in the core_tree (RB_EMPTY_NODE returns false) despite being sched_delayed. The driver should report `FAIL`.

On the **fixed kernel**, the `sched_core_enqueue()` early-return guard prevents the task from being added to the core_tree while sched_delayed is set. The task will NOT be in the core_tree. The driver should report `PASS`.

**Additional detection:** Also verify `core_task_seq` behavior. Record `rq->core->core_task_seq` before and after the migration. On the buggy kernel, the sequence number will be incremented by 2 (once for the spurious dequeue, once for the spurious enqueue). On the fixed kernel, the sequence number will remain unchanged (both operations are skipped due to the sched_delayed guard).

**kSTEP changes needed:** The main extension required is a helper function or pattern for setting `core_cookie` on a kSTEP-created task. This involves:
1. `KSYM_IMPORT(sched_core_get)` and `KSYM_IMPORT(sched_core_put)` — these are non-static global functions in `kernel/sched/core.c`.
2. Direct manipulation of `p->core_cookie` with proper locking (task_rq_lock / task_rq_unlock from `sched.h` internals, accessible via `#include "internal.h"`).
3. Manual calls to `sched_core_enqueue()`/`sched_core_dequeue()` around the cookie change (these are non-static functions declared in `sched.h`).

This is a minor extension — no new framework APIs are strictly required, only KSYM_IMPORT usage and direct struct field writes with proper locking, which is already a pattern used in other kSTEP drivers.
