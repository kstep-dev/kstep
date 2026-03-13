# EEVDF: NEXT_BUDDY Selects Delayed Entity as Buddy

**Commit:** `493afbd187c4c9cc1642792c0d9ba400c3d6d90d`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.13-rc3
**Buggy since:** v6.12-rc1 (introduced by `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

When the `NEXT_BUDDY` scheduler feature is enabled (via `/sys/kernel/debug/sched/features`), the kernel can set a sched entity that has been marked as `sched_delayed` as the CFS run queue's `->next` buddy. The delayed dequeue mechanism, introduced in commit `152e11f6df29`, allows tasks that go to sleep to remain on the run queue in a "delayed" state (`se->sched_delayed = 1`) until they become ineligible, deferring the expensive dequeue operation. However, this introduced an invariant violation: when `pick_next_entity()` checks the `->next` buddy (with `NEXT_BUDDY` enabled), it asserts via `SCHED_WARN_ON(cfs_rq->next->sched_delayed)` that the buddy must not be in the delayed state, since a delayed entity is conceptually dequeued (going to sleep) and should not be picked to run next.

The bug manifests through two distinct code paths. First, `check_preempt_wakeup_fair()` unconditionally calls `set_next_buddy(pse)` for newly woken tasks when `NEXT_BUDDY` is enabled, without checking whether the woken entity's `sched_delayed` flag is set. This is relevant because delayed entities can be re-enqueued through the load balancing path (e.g., `attach_tasks()`) which calls `wakeup_preempt()`, potentially setting a delayed entity as the next buddy. Second, when `dequeue_entity()` decides to delay the dequeue of an entity (setting `se->sched_delayed = 1`), the original code only cleared the `->next` buddy for that specific entity with a narrow `if (cfs_rq->next == se) cfs_rq->next = NULL;` check, but this happened after the delayed dequeue decision point, not comprehensively enough.

The combination of these two issues means that a delayed entity can become and remain the `->next` buddy on a CFS run queue, leading to a cascade of failures when the scheduler tries to pick the next task.

## Root Cause

The root cause has two parts operating in tandem:

**Part 1 — `check_preempt_wakeup_fair()` sets delayed entities as NEXT_BUDDY:** In the buggy code, `check_preempt_wakeup_fair()` at line 8777 contains:
```c
if (sched_feat(NEXT_BUDDY) && !(wake_flags & WF_FORK)) {
    set_next_buddy(pse);
}
```
This unconditionally calls `set_next_buddy()` for any newly woken entity. The `set_next_buddy()` function walks up the hierarchy via `for_each_sched_entity(se)` and sets `cfs_rq_of(se)->next = se` for each level. It checks `!se->on_rq` and `se_is_idle(se)` as bail-out conditions, but does NOT check `se->sched_delayed`. Since delayed entities still have `se->on_rq == 1` (they remain on the run queue), the `!se->on_rq` check passes, and the delayed entity gets set as the `->next` buddy.

This scenario is triggered in the load balancing path: when `attach_tasks()` migrates a delayed task to a new CPU, the enqueue path calls `wakeup_preempt()` → `check_preempt_wakeup_fair()` → `set_next_buddy(pse)`. K Prateek Nayak confirmed via SCHED_WARN_ON instrumentation that this load-balancing path is the primary trigger.

**Part 2 — `clear_buddies()` called too late in `dequeue_entity()`:** In the buggy code, when `dequeue_entity()` decides to delay a dequeue, the sequence is:
```c
dequeue_entity(cfs_rq, se, flags) {
    update_curr(cfs_rq);
    // ... delayed dequeue decision ...
    if (sched_feat(DELAY_DEQUEUE) && delay && !entity_eligible(cfs_rq, se)) {
        if (cfs_rq->next == se)
            cfs_rq->next = NULL;      // narrow clear
        se->sched_delayed = 1;
        return false;
    }
    // ... much later in the function ...
    clear_buddies(cfs_rq, se);         // full clear (never reached for delayed)
}
```
When a dequeue is delayed (returns false early), only the narrow `if (cfs_rq->next == se) cfs_rq->next = NULL` runs. The comprehensive `clear_buddies()` call, which is the proper buddy-clearing mechanism, appears much later in the function and is never reached for delayed dequeues. While the narrow clear handles the local `cfs_rq`, it does not address the full hierarchy that `clear_buddies()` → `__clear_buddies_next()` properly walks. More critically, after the entity is marked `sched_delayed`, nothing prevents it from being set as the next buddy again (until the entity is actually dequeued).

## Consequence

The observable consequences are severe and escalate progressively:

**1. Kernel WARNING:** When `pick_next_entity()` finds `cfs_rq->next` is set and the entity is eligible, it hits the assertion `SCHED_WARN_ON(cfs_rq->next->sched_delayed)`, producing a kernel warning with the message `cfs_rq->next->sched_delayed`. The stack trace shows `pick_task_fair+0x130/0x150` → `pick_next_task_fair` → `__pick_next_task` → `pick_next_task` → `__schedule`. This was observed on CPU 51 in a kworker thread (PID 2150).

**2. RCU stalls and PSI inconsistencies:** Because a delayed (effectively sleeping) entity is being selected to run, accounting becomes inconsistent. K Prateek Nayak observed PSI (Pressure Stall Information) splats: `psi: inconsistent task state! task=2524:kworker/u1028:2 cpu=154 psi_flags=10 clear=14 set=0` — the PSI flags indicate that a dequeued entity was picked for execution before its proper wakeup, causing `nr_running` to go awry. This leads to RCU grace-period kthread starvation, producing `rcu: INFO: rcu_preempt detected stalls on CPUs/tasks` messages with stall durations of 15000+ jiffies.

**3. NULL pointer dereference and kernel panic:** In the worst case, the incorrect nr_running accounting causes `pick_eevdf()` to return NULL. This happens because: (a) `curr` is going to sleep so `curr` is set to NULL in `pick_eevdf()`, (b) the rb-tree walk finds no eligible `best` because the run queue is effectively empty despite `nr_running > 0`, and (c) the code `if (!best || (curr && entity_before(curr, best))) best = curr;` leaves `best = NULL`. When `pick_next_entity()` then dereferences `se->sched_delayed` on the NULL return value, it triggers a NULL pointer dereference at virtual address `0x0000000000000051` (offsetof sched_delayed in sched_entity), causing an immediate kernel panic: `Kernel panic - not syncing: Oops: Fatal exception`.

## Fix Summary

The fix addresses both root causes with two changes to `kernel/sched/fair.c`:

**Change 1 — Move `clear_buddies()` before the delayed dequeue decision in `dequeue_entity()`:** The call to `clear_buddies(cfs_rq, se)` is moved from its original position (after `update_stats_dequeue_fair()`) to immediately after `update_curr(cfs_rq)`, before the `DEQUEUE_DELAYED` / `DELAY_DEQUEUE` decision block. This ensures that any entity entering the dequeue path always has its buddy status cleared, regardless of whether the dequeue is delayed or immediate. The narrow `if (cfs_rq->next == se) cfs_rq->next = NULL;` in the delayed dequeue path is removed since `clear_buddies()` already handles it comprehensively. This prevents a race where an entity is marked delayed but still has its `->next` buddy pointer set in some parent cfs_rq.

**Change 2 — Guard `set_next_buddy()` against delayed entities in `check_preempt_wakeup_fair()`:** The condition in `check_preempt_wakeup_fair()` is changed from:
```c
if (sched_feat(NEXT_BUDDY) && !(wake_flags & WF_FORK))
```
to:
```c
if (sched_feat(NEXT_BUDDY) && !(wake_flags & WF_FORK) && !pse->sched_delayed)
```
This prevents any delayed entity from being nominated as the next buddy in the first place. Combined with Change 1, this establishes the invariant: no `->next` buddy is ever in the `sched_delayed` state.

The two changes together are both necessary and sufficient: Change 1 ensures that entities transitioning into the delayed state lose their buddy status, and Change 2 ensures that entities already in the delayed state are never nominated as buddies. This fully preserves the assertion in `pick_next_entity()`.

## Triggering Conditions

The bug requires the following specific conditions:

- **NEXT_BUDDY sched feature enabled:** This feature is disabled by default (`SCHED_FEAT(NEXT_BUDDY, false)` in `kernel/sched/features.h`). It must be explicitly enabled via `echo NEXT_BUDDY > /sys/kernel/debug/sched/features` (or `echo "NEXT_BUDDY" > /sys/kernel/debug/sched_features` on older interfaces). Without NEXT_BUDDY, the `cfs_rq->next` buddy is never set, and the buggy code path in `check_preempt_wakeup_fair()` is never reached.

- **DELAY_DEQUEUE sched feature enabled (default):** The delayed dequeue mechanism must be active (`SCHED_FEAT(DELAY_DEQUEUE, true)` — enabled by default since v6.12-rc1). This is what creates entities in the `sched_delayed` state.

- **Multiple CPUs:** The primary trigger path involves load balancing (`attach_tasks()`) pulling a delayed task from one CPU to another. This requires at least 2 CPUs. The original report was on a 384-CPU system, but the bug can be reproduced on systems with as few as 2 CPUs.

- **Task churn with sleep/wakeup patterns:** Tasks must be going through sleep → delayed dequeue → wakeup cycles while load balancing is active. Workloads like `perf bench sched messaging` (which K Prateek Nayak confirmed as a reproducer) or SPECjbb create sufficient task churn. The key pattern is: (1) a task goes to sleep and gets delay-dequeued, (2) load balancing migrates the delayed task, (3) the enqueue on the destination CPU triggers `check_preempt_wakeup_fair()`, and (4) the delayed entity is set as the `->next` buddy.

- **Timing:** The bug is not a strict race condition — it reliably occurs whenever the above conditions are met. Once `NEXT_BUDDY` is enabled on a kernel with `DELAY_DEQUEUE`, any significant scheduling workload will trigger it within seconds to minutes. The original reporter saw the warning at ~125 seconds into a SPECjbb run.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP. The key requirement is enabling the `NEXT_BUDDY` sched feature and creating conditions where a delayed entity is set as the next buddy. Here is a concrete step-by-step plan:

**Step 1 — Enable required sched features:** Use `kstep_write()` (the underlying raw write function) to enable `NEXT_BUDDY` via debugfs:
```c
kstep_write("/sys/kernel/debug/sched/features", "NEXT_BUDDY\n", 11);
```
`DELAY_DEQUEUE` is enabled by default since v6.12-rc1, so no action is needed for that. Verify both features are active by reading the features file.

**Step 2 — Configure topology:** Use at least 2 CPUs (`kstep_topo_init()` or QEMU `--smp 4`). The load balancing path is the primary trigger, so having multiple CPUs is essential. A simple 4-CPU configuration would provide adequate opportunity for load balancing:
```c
kstep_topo_init();
kstep_topo_apply();
```

**Step 3 — Create multiple CFS tasks:** Create several CFS tasks (e.g., 4-8 tasks) that repeatedly block and wake up. This creates the task churn needed for delayed dequeues:
```c
struct task_struct *tasks[8];
for (int i = 0; i < 8; i++) {
    tasks[i] = kstep_task_create();
}
```

**Step 4 — Create sleep/wakeup churn to trigger delayed dequeue:** Put tasks to sleep (which triggers delayed dequeue if they're not eligible) and wake them up. The key is creating entities in the `sched_delayed` state:
```c
for (int i = 0; i < 8; i++) {
    kstep_task_block(tasks[i]);  // task goes to sleep
}
kstep_tick_repeat(5);  // let time pass, tasks become ineligible → delayed
for (int i = 0; i < 8; i++) {
    kstep_task_wakeup(tasks[i]);  // wakeup triggers check_preempt_wakeup_fair
}
```

**Step 5 — Trigger load balancing to cause migration of delayed entities:** The primary trigger path is `attach_tasks()` in the load balancer pulling a delayed task. Use pinning to force tasks onto specific CPUs, then release them to allow migration:
```c
// Pin tasks to CPU 1 initially
for (int i = 0; i < 8; i++) {
    kstep_task_pin(tasks[i], 1, 2);  // pin to CPU 1
}
kstep_tick_repeat(10);
// Create imbalance by blocking some tasks and adjusting affinity
for (int i = 0; i < 4; i++) {
    kstep_task_block(tasks[i]);
}
kstep_tick_repeat(5);
// Unpin to allow load balancer to migrate
for (int i = 0; i < 8; i++) {
    kstep_task_pin(tasks[i], 1, 4);  // allow CPUs 1-3
}
// Wake blocked tasks — these may be delay-dequeued
for (int i = 0; i < 4; i++) {
    kstep_task_wakeup(tasks[i]);
}
kstep_tick_repeat(20);  // trigger load balancing
```

**Step 6 — Use callbacks to detect the bug:** Register an `on_tick_begin` or `on_tick_end` callback to check for the invariant violation. The detection checks whether any CFS run queue has a `->next` buddy that is in the `sched_delayed` state:
```c
static int on_tick_check(void) {
    for (int cpu = 0; cpu < nr_cpu_ids; cpu++) {
        struct rq *rq = cpu_rq(cpu);
        struct cfs_rq *cfs_rq = &rq->cfs;
        if (cfs_rq->next && cfs_rq->next->sched_delayed) {
            kstep_fail("BUG: cfs_rq->next (CPU %d) has sched_delayed set!", cpu);
            return 1;
        }
    }
    return 0;
}
```
Assign this to `on_tick_end`.

**Step 7 — Run repeated cycles:** Run many sleep/wakeup/tick cycles to maximize the chance of triggering the bug. A loop of 50-100 iterations should suffice:
```c
for (int cycle = 0; cycle < 100; cycle++) {
    for (int i = 0; i < 8; i++)
        kstep_task_block(tasks[i]);
    kstep_tick_repeat(3);
    for (int i = 0; i < 8; i++)
        kstep_task_wakeup(tasks[i]);
    kstep_tick_repeat(5);
}
```

**Step 8 — Pass/fail criteria:** On the **buggy kernel** (pre-fix), the `on_tick_end` callback should detect `cfs_rq->next->sched_delayed != 0` and call `kstep_fail()`. Additionally, the kernel may print the `SCHED_WARN_ON(cfs_rq->next->sched_delayed)` warning in dmesg. On the **fixed kernel**, the `->next` buddy should never be a delayed entity, and the test should complete all cycles and call `kstep_pass()`.

**kSTEP extension needed:** The main requirement is writing to `/sys/kernel/debug/sched/features` to enable `NEXT_BUDDY`. The existing `kstep_write()` function in `kmod/kernel.c` can write to any file path, so this is directly usable from a driver. No framework changes are needed — just call `kstep_write("/sys/kernel/debug/sched/features", "NEXT_BUDDY\n", 11)` from the driver. Access to `cfs_rq->next` and `se->sched_delayed` fields is available through kSTEP's internal access (`kmod/internal.h` provides `cpu_rq()` and all `kernel/sched/sched.h` internals).

**Alternative simpler approach:** Instead of relying on load balancing migration, a simpler approach would directly use `KSYM_IMPORT` to access `set_next_buddy` and call it on a delayed entity, then trigger `pick_next_entity()` through a tick. However, triggering it through the natural scheduler path (load balancing + wakeup) is more faithful to the real-world bug trigger.
