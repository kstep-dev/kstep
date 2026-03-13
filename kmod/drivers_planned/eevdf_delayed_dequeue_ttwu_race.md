# EEVDF: Delayed Dequeue pick_next_task_fair vs try_to_wake_up Race

**Commit:** `b55945c500c5723992504aa03b362fab416863a6`
**Affected files:** `kernel/sched/fair.c`, `kernel/sched/sched.h`
**Fixed in:** v6.12-rc6
**Buggy since:** v6.12-rc1 (introduced by `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

The "delayed dequeue" feature introduced in commit `152e11f6df29` allows CFS/EEVDF tasks to remain on the runqueue even after they have logically blocked (sleeping). Instead of immediately dequeuing the task when it blocks, the scheduler marks it as `sched_delayed` and leaves it on the runqueue. The actual dequeue happens later — specifically, when `pick_next_entity()` selects this delayed task as the next to run and discovers it is `sched_delayed`, it then calls `dequeue_entities()` with `DEQUEUE_SLEEP | DEQUEUE_DELAYED` to finally remove it. This deferred removal triggers `__block_task()`, which sets `p->on_rq = 0`.

The bug is a race condition between `pick_next_task_fair()` (which calls `pick_next_entity()` → `dequeue_entities()` → `__block_task()`) and `try_to_wake_up()` (ttwu) running concurrently on a different CPU. The root of the problem is that `__block_task()` writes `p->on_rq = 0` using `WRITE_ONCE()`, but in the buggy code this store appears **before** other operations that still reference `@p`. Once `p->on_rq` is set to 0, the task is visible to `try_to_wake_up()` as "not queued", which allows ttwu to swoop in, migrate the task to a different CPU, and set `p->on_rq = 1` — all while the original CPU still holds its own `rq->__lock` and continues to access `@p` thinking it still owns it.

This was reported by KCSAN (Kernel Concurrency Sanitizer) via syzkaller, which detected that both `pick_next_task_fair()` and `try_to_wake_up()` were concurrently writing to the same `p->on_rq` field. The `ASSERT_EXCLUSIVE_WRITER(p->on_rq)` assertion in `__block_task()` fired because both code paths were holding **different** `rq->__lock` instances (one for the old CPU's rq, one for the new CPU's rq after migration), violating the invariant that only one writer should modify `p->on_rq` at a time.

The bug was also independently reproduced by Alexander Potapenko on ARM while fuzzing KVM, and by Marco Elver who obtained the full two-sided stack trace showing the concurrent write from `activate_task()` (via ttwu) and the assert from `__block_task()` (via `dequeue_entities()`).

## Root Cause

The root cause lies in the ordering of operations in `__block_task()` and the subsequent use of `@p` / `@se` after `__block_task()` returns. In the buggy code, `__block_task()` was defined as:

```c
static inline void __block_task(struct rq *rq, struct task_struct *p)
{
    WRITE_ONCE(p->on_rq, 0);        // (A) Release the task
    ASSERT_EXCLUSIVE_WRITER(p->on_rq);
    if (p->sched_contributes_to_load)
        rq->nr_uninterruptible++;
    if (p->in_iowait) {
        atomic_inc(&rq->nr_iowait);
        delayacct_blkio_start();
    }
}
```

The critical problem is at point (A): once `p->on_rq` is stored as 0, the `try_to_wake_up()` function on another CPU can observe this via its `p->on_rq` load (protected by `p->pi_lock`, not `rq->__lock`). The ttwu path then proceeds to select a new CPU via `select_task_rq()`, calls `set_task_cpu(p, cpu)` to migrate the task, and then calls `ttwu_queue()` → `ttwu_do_activate()` → `activate_task()`, which locks the **new** CPU's `rq->__lock` and writes `WRITE_ONCE(p->on_rq, TASK_ON_RQ_QUEUED)`. At this point, both the old CPU (still in `__schedule()` → `pick_next_task_fair()`) and the new CPU (in ttwu) are writing to `p->on_rq`, each holding a different `rq->__lock`.

The race is visible in two call sites that reference `@p` after `__block_task()`:

**1. `pick_next_entity()` in fair.c:**
```c
static struct sched_entity *pick_next_entity(struct rq *rq, struct cfs_rq *cfs_rq)
{
    struct sched_entity *se = pick_eevdf(cfs_rq);
    if (se->sched_delayed) {
        dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
        // __block_task() sets p->on_rq = 0 inside dequeue_entities
        SCHED_WARN_ON(se->sched_delayed);  // BUG: accesses @se after release
        SCHED_WARN_ON(se->on_rq);          // BUG: accesses @se after release
        return NULL;
    }
    return se;
}
```

After `dequeue_entities()` calls `__block_task()` which sets `p->on_rq = 0`, the task `@p` (and its embedded `@se`) may already be migrated to another CPU by ttwu. The subsequent `SCHED_WARN_ON(se->sched_delayed)` and `SCHED_WARN_ON(se->on_rq)` accesses are therefore unsynchronized — they read fields of a task_struct that is now on a different runqueue, protected by a different lock.

**2. `dequeue_task_fair()` in fair.c:**
```c
static bool dequeue_task_fair(struct rq *rq, struct task_struct *p, int flags)
{
    if (!(p->se.sched_delayed && (task_on_rq_migrating(p) || (flags & DEQUEUE_SAVE))))
        util_est_dequeue(&rq->cfs, p);

    if (dequeue_entities(rq, &p->se, flags) < 0) {
        util_est_update(&rq->cfs, p, DEQUEUE_SLEEP);  // BUG: accesses @p after release
        return false;
    }

    util_est_update(&rq->cfs, p, flags & DEQUEUE_SLEEP);  // BUG: accesses @p
    hrtick_update(rq);
    return true;
}
```

When `dequeue_entities()` returns with `task_delayed` true (the `if (p && task_delayed)` branch), it calls `__block_task(rq, p)` as the last action. But in the buggy code, `util_est_update()` was called **after** `dequeue_entities()` returned, meaning it accessed `@p` after `__block_task()` had already released it. Similarly, `hrtick_update(rq)` accesses the rq but this was reordered in the fix as well.

**3. `dequeue_entities()` itself:**
Inside `dequeue_entities()`, the buggy code had:
```c
if (p && task_delayed) {
    hrtick_update(rq);      // Called before __block_task
    __block_task(rq, p);    // Releases @p
}
```
The fix moves `hrtick_update(rq)` to before `__block_task(rq, p)` and adds a comment that `__block_task` must be last because `@p` might not be valid after it.

## Consequence

The primary consequence is a **data race / use-after-release** on the task_struct. While this does not directly cause a kernel panic in most cases (the task_struct is not freed, just migrated), it leads to several dangerous outcomes:

**KCSAN assertion failures:** The `ASSERT_EXCLUSIVE_WRITER(p->on_rq)` fires, producing a BUG message in the kernel log. The KCSAN reports show `__block_task()` on one CPU asserting exclusive write access to `p->on_rq` while `activate_task()` on another CPU concurrently writes `p->on_rq = TASK_ON_RQ_QUEUED`. The stack traces from KCSAN show:
```
BUG: KCSAN: assert: race in dequeue_entities / ttwu_do_activate

write (marked) to 0xffff9e100329c628 of 4 bytes by interrupt on cpu 0:
 activate_task kernel/sched/core.c:2064 [inline]
 ttwu_do_activate+0x153/0x3e0 kernel/sched/core.c:3671
 try_to_wake_up+0x60f/0xaf0 kernel/sched/core.c:4270

assert no writes to 0xffff9e100329c628 of 4 bytes by task 10571 on cpu 3:
 __block_task kernel/sched/sched.h:2770 [inline]
 dequeue_entities+0xd83/0xe70 kernel/sched/fair.c:7177
 pick_next_entity kernel/sched/fair.c:5627 [inline]
 pick_next_task_fair+0xaf/0x710 kernel/sched/fair.c:8876
```

**Potential corruption of util_est / scheduling state:** The `util_est_update()` call after `__block_task()` in `dequeue_task_fair()` accesses `@p` to update utilization estimation. If the task has already been migrated, this could corrupt the util_est data for the wrong CFS runqueue, leading to incorrect CPU frequency scaling decisions or load balancing.

**Potential for worse races:** In theory, if the task were to be freed between the `__block_task()` store and the subsequent `@p` accesses (e.g., if the task exits and its task_struct is reclaimed), this could become a use-after-free. While this is unlikely under normal conditions (the task is being woken up, so it's not exiting), it represents a class of bug that could become exploitable.

## Fix Summary

The fix makes three coordinated changes to ensure that `__block_task()` is the absolute last operation that touches `@p`, and that no code after `__block_task()` references the task:

**1. Reorder `__block_task()` to store `p->on_rq = 0` last (sched.h):**
The `WRITE_ONCE(p->on_rq, 0)` is moved from the beginning to the end of `__block_task()`, and changed to `smp_store_release(&p->on_rq, 0)`. All the bookkeeping operations (`nr_uninterruptible++`, `nr_iowait++`, `delayacct_blkio_start()`) now happen **before** the releasing store. The `ASSERT_EXCLUSIVE_WRITER` is also moved before the store. The `smp_store_release` ensures that all prior memory operations are visible before the `p->on_rq = 0` becomes visible to other CPUs. A detailed comment documents the exact interleaving between `__schedule()` and `try_to_wake_up()` that makes this ordering critical.

**2. Remove post-`__block_task()` references in `pick_next_entity()` (fair.c):**
The two `SCHED_WARN_ON` checks after `dequeue_entities()` are removed:
```c
// Before (buggy):
dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
SCHED_WARN_ON(se->sched_delayed);
SCHED_WARN_ON(se->on_rq);
return NULL;

// After (fixed):
dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED);
/* Must not reference @se again, see __block_task(). */
return NULL;
```

**3. Reorder `util_est_update()` and add `__block_task()` last-call guarantee in `dequeue_task_fair()` (fair.c):**
The `util_est_update()` call is moved to **before** `dequeue_entities()` so it no longer references `@p` after `__block_task()` may have fired. The comment `"Must not reference @p after dequeue_entities(DEQUEUE_DELAYED)"` is added. Inside `dequeue_entities()`, `__block_task(rq, p)` is explicitly documented as the last operation with the comment `"Must be last, @p might not be valid after this."` The `hrtick_update(rq)` call that was previously after `__block_task` in `dequeue_entities` is moved to `dequeue_task_fair`.

This fix is correct and complete because it establishes a clear ownership boundary: once `__block_task()` performs `smp_store_release(&p->on_rq, 0)`, the calling CPU relinquishes all rights to `@p`. The `smp_store_release` provides the memory ordering guarantee that ttwu's `smp_load_acquire`-style control dependency (which reads `p->on_rq` under `p->pi_lock`) will see a consistent state.

## Triggering Conditions

The bug requires the following precise conditions:

- **SMP system with at least 2 CPUs:** The race is between two CPUs — one running `__schedule()` → `pick_next_task_fair()` and another running `try_to_wake_up()`. Both must execute concurrently.

- **Delayed dequeue feature active (CONFIG_SCHED_FAIR_GROUP_SCHED or default CFS):** The delayed dequeue mechanism (introduced in v6.12-rc1) must be in effect. A CFS task must have its `sched_delayed` flag set, meaning it previously blocked but was left on the runqueue instead of being immediately dequeued.

- **A task in `sched_delayed` state:** A CFS task must have called `schedule()` (e.g., by sleeping on a timer, blocking on I/O, or waiting on a poll/select) such that it enters the delayed dequeue path. The task's `se.sched_delayed` flag is set to 1.

- **`pick_eevdf()` selects the delayed task:** When another task (or the idle task) on the same CPU calls `__schedule()` → `pick_next_task_fair()` → `pick_next_entity()`, the EEVDF algorithm must select the `sched_delayed` task as the next entity. This triggers the dequeue path.

- **Concurrent wakeup of the same task from another CPU:** A different CPU must call `try_to_wake_up()` on the same task at approximately the same time. The ttwu path reads `p->on_rq` while holding `p->pi_lock` (not the rq lock). If the `p->on_rq = 0` store from `__block_task()` is visible to ttwu before the pick_next path finishes its post-`__block_task()` accesses, the race is triggered.

- **Timing window:** The race window is extremely narrow — it is the time between when `__block_task()` writes `p->on_rq = 0` and when the subsequent code in `pick_next_entity()` or `dequeue_task_fair()` accesses `@se`/`@p`. This is only a few instructions wide. The fact that syzkaller could trigger it (with KCSAN) but could not produce a reliable reproducer demonstrates how tight the window is.

- **No specific kernel configuration beyond KCSAN for detection:** The actual data race occurs regardless of KCSAN, but without KCSAN the consequences are usually silent corruption (util_est mismatch, stale reads) rather than an assertion failure. The race is most likely to manifest on systems under heavy scheduler pressure with many tasks waking and sleeping rapidly.

## Reproduce Strategy (kSTEP)

The core challenge in reproducing this bug with kSTEP is that it is fundamentally a **SMP race condition** with an extremely narrow window. However, we can design a kSTEP driver that creates the conditions for the race and uses internal state inspection to detect the buggy behavior. The strategy has two complementary approaches:

**Approach 1: Verify the code path and demonstrate delayed dequeue + concurrent wakeup**

1. **Topology setup:** Configure QEMU with at least 2 CPUs. Use `kstep_topo_init()` and configure a simple SMP topology.

2. **Create the delayed-dequeue task:** Create a CFS task `p1` with `kstep_task_create()` and pin it to CPU 1 with `kstep_task_pin(p1, 1, 1)`. Let it run for several ticks to establish vruntime.

3. **Trigger delayed dequeue:** Use `kstep_task_block(p1)` to put the task to sleep. With the delayed dequeue feature, this should set `p1->se.sched_delayed = 1` and leave it on the runqueue. Use `KSYM_IMPORT` to access `p1->se.sched_delayed` and verify it is set.

4. **Create waker task on another CPU:** Create a kthread `waker` with `kstep_kthread_create("waker")` and bind it to CPU 1 (same CPU as `p1`). Also create observer tasks on CPU 1 to ensure EEVDF picks the delayed entity.

5. **Trigger pick_next_entity to select the delayed task:** Use `kstep_tick()` repeatedly on CPU 1 to cause a reschedule. When `pick_next_entity()` runs on CPU 1 and selects the `sched_delayed` entity, it will call `dequeue_entities()` → `__block_task()`.

6. **Concurrent wakeup:** Use `kstep_kthread_syncwake()` from a kthread on CPU 2 to wake `p1` at approximately the same time. While the exact interleaving cannot be controlled at instruction granularity, running many iterations increases the probability of hitting the window.

7. **Detection:** After each iteration, check `p1->on_rq` and `task_cpu(p1)`. On the buggy kernel, if the race is hit, `p1` may end up migrated to CPU 2 while CPU 1's `pick_next_entity` still accesses `p1->se.sched_delayed` and `p1->se.on_rq`. Use the `on_tick_begin` callback to inspect state at critical points.

**Approach 2: Directly verify the post-`__block_task()` access pattern**

Rather than trying to hit the exact race window, we can verify that the **buggy code accesses @se after __block_task()** by instrumenting the dequeue path:

1. **Setup:** Create 2+ CFS tasks on CPU 1, one of which will be delayed-dequeued.

2. **Import key symbols:** Use `KSYM_IMPORT` to access internal scheduler structures: `struct cfs_rq`, `struct sched_entity`, `p->on_rq`, `se->sched_delayed`.

3. **Trigger delayed dequeue via pick_next:** Block one task to make it `sched_delayed`, then advance ticks until `pick_next_entity()` selects it. At this point, on the **buggy kernel**, `pick_next_entity` will:
   - Call `dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED)`
   - Inside, `__block_task()` writes `p->on_rq = 0` (with `WRITE_ONCE`)
   - Return to `pick_next_entity` which then reads `se->sched_delayed` and `se->on_rq`

4. **Use `on_tick_end` callback** to check state after each tick. Verify that on the buggy kernel, the delayed task's `on_rq` transitions to 0 during pick_next while `sched_delayed` may still be set. On the fixed kernel, the `smp_store_release` ordering and removal of post-checks ensure cleaner state transitions.

**Approach 3: Stress test with repeated block/wake cycles**

1. **Create many tasks (e.g., 4-8) on 2 CPUs.** Half on CPU 1, half on CPU 2.

2. **Rapidly cycle tasks through block → delayed_dequeue → wakeup.** Use a loop that repeatedly:
   - Blocks a task on CPU 1 (entering delayed dequeue)
   - Immediately wakes it from CPU 2 via `kstep_kthread_syncwake`
   - Advances one tick to trigger pick_next

3. **After each cycle, verify consistency:**
   - `p->on_rq` should be either 0 or 1, never 2 (TASK_ON_RQ_MIGRATING set concurrently)
   - The task should be on the expected CPU's runqueue
   - `rq->nr_running` should be consistent

4. **Pass/fail criteria:**
   - On the **buggy kernel**: Under enough iterations, we may observe that `p->on_rq` is read as inconsistent from the pick_next_entity path (the `SCHED_WARN_ON` would have fired, but since it's removed in the fix, we check for `se->on_rq != 0` after delayed dequeue — if the race hits, `on_rq` becomes 1 due to ttwu setting it)
   - On the **fixed kernel**: The `smp_store_release` and reordering ensure that no code accesses `@p` after the releasing store, so the task state is always consistent from the local CPU's perspective

**kSTEP modifications needed:** None required. The existing `kstep_task_block()`, `kstep_kthread_syncwake()`, `kstep_tick()`, and `KSYM_IMPORT()` APIs are sufficient to create delayed dequeue scenarios and trigger concurrent wakeups. The main challenge is the probabilistic nature of the race — many iterations may be needed.

**Expected behavior:**
- **Buggy kernel:** Under repeated iterations, KCSAN (if enabled) fires the `ASSERT_EXCLUSIVE_WRITER` assertion. Without KCSAN, the race manifests as the pick_next_entity path reading `se->on_rq` as non-zero after `__block_task()` set it to 0 (because ttwu re-set it to 1 concurrently). This can be detected by placing a read of `se->on_rq` immediately after the `dequeue_entities` call in a callback or by checking task CPU migration patterns.
- **Fixed kernel:** The `smp_store_release(&p->on_rq, 0)` is the last operation in `__block_task()`, and no code after it references `@p`. Even if ttwu races in, the local CPU never touches `@p` again, so no inconsistency is observed.

**Note on determinism:** This race is inherently non-deterministic. The kSTEP driver should run multiple iterations (e.g., 100-1000 block/wake cycles) and use statistical detection. The driver may not reproduce the exact KCSAN splat without KCSAN enabled, but can verify the structural fix by confirming that delayed dequeue + immediate wakeup produces consistent state on the fixed kernel.
