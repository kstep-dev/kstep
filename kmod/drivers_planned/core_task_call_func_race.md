# Core: Race Between task_call_func() and __schedule() Allows Callback on Mid-Switch Task

**Commit:** `91dabf33ae5df271da63e87ad7833e5fdb4a44b9`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.1-rc6
**Buggy since:** v6.1-rc1 (introduced by `f5d39b020809` "freezer,sched: Rewrite core freezer logic")

## Bug Description

There is a race condition between `__schedule()` executing a context switch and
`task_call_func()` being called from another CPU on the same task. The
`task_call_func()` function is designed to invoke a callback on a task in a
"fixed" state — meaning the task should be pinned in a well-defined scheduling
state (blocked, queued, or running) with the appropriate lock held. However, the
buggy implementation fails to account for the intermediate state where a task has
been dequeued from the runqueue (`on_rq = 0`) but has not yet completed its
context switch (`on_cpu = 1`).

The function `task_call_func()` decides whether it needs to acquire `rq->lock`
based on the task's `__state` and `on_rq` fields. If the task appears to be in a
sleeping state (not `TASK_RUNNING` or `TASK_WAKING`) and is not on the runqueue
(`on_rq == 0`), the buggy code assumes the task is fully blocked and calls the
callback function without holding `rq->lock`. But during `__schedule()`, there
is a window where the task has already been deactivated (`deactivate_task()` sets
`on_rq = 0`) but is still physically executing on the CPU — it hasn't finished
the context switch yet. During this window, the task's `on_cpu` is still 1, and
it may still hold `rq->lock` (or have recently held it). Calling the callback
without `rq->lock` in this state leads to observing inconsistent scheduler state.

This bug was introduced by commit `f5d39b020809` ("freezer,sched: Rewrite core
freezer logic") which added `__freeze_task()` → `task_call_func(p,
__set_task_frozen, NULL)` as a new user of `task_call_func()`. Prior users of
`task_call_func()` (e.g., livepatch, RCU tasks trace) apparently did not care
about this particular edge case, but `__set_task_frozen()` does because it
checks `lockdep_depth` and modifies `p->__state`, both of which are sensitive to
the task still being mid-schedule on another CPU.

## Root Cause

The root cause is in the pre-fix version of `task_call_func()` in
`kernel/sched/core.c`. The function determines whether `rq->lock` is needed with
a simple inline check:

```c
state = READ_ONCE(p->__state);
smp_rmb();
if (state == TASK_RUNNING || state == TASK_WAKING || p->on_rq)
    rq = __task_rq_lock(p, &rf);
```

When `state` is a sleep state (e.g., `TASK_INTERRUPTIBLE`) and `on_rq == 0`,
this check decides no `rq->lock` is needed and proceeds directly to calling
`func(p, arg)`. The assumption is that a sleeping, dequeued task is fully
quiescent and will not be concurrently modified by the scheduler.

However, this assumption is violated during `__schedule()` on the task's CPU.
The execution flow on CPU1 when task T is being scheduled out is:

1. `rq_lock(rq, &rf)` — acquires `rq->lock`
2. `prev_state = READ_ONCE(prev->__state)` — reads sleep state
3. `deactivate_task(rq, prev, DEQUEUE_SLEEP)` — sets `prev->on_rq = 0`
4. `pick_next_task(rq, prev, &rf)` — selects next task to run
5. `RCU_INIT_POINTER(rq->curr, next)` — updates `rq->curr` to next task,
   making `task_curr(prev)` return `false`
6. `context_switch(rq, prev, next, &rf)`:
   a. `prepare_task_switch()` → `prepare_task(next)` sets `next->on_cpu = 1`
   b. MM switching code (enter_lazy_tlb, switch_mm, etc.)
   c. `prepare_lock_switch()` → `spin_release()` — prev's `lockdep_depth`
      decremented
   d. `switch_to(prev, next, prev)` — actual register/stack switch
7. On next's stack: `finish_task_switch(prev)`:
   a. `finish_task(prev)` → `smp_store_release(&prev->on_cpu, 0)` — clears
      prev's `on_cpu`
   b. `finish_lock_switch(rq)` → releases `rq->lock`

The critical race window is **from step 3 (deactivate_task) to step 7a
(finish_task)**. During this entire window:

- `prev->__state` = sleep state (not `TASK_RUNNING`, not `TASK_WAKING`)
- `prev->on_rq` = 0
- `prev->on_cpu` = 1

The buggy `task_call_func()` on CPU0, seeing `state == sleep` and `on_rq == 0`,
skips `rq->lock` and invokes the callback immediately.

For the specific `__set_task_frozen()` callback, there is an additional critical
sub-window between steps 5 and 6c:

- After step 5: `task_curr(prev) == false` (because `rq->curr = next`)
- Before step 6c: `prev->lockdep_depth > 0` (rq->lock's `spin_release` hasn't
  happened yet)

In this sub-window, `__set_task_frozen()` passes the `task_curr(p)` guard check
(returns false), reaches the lockdep check `WARN_ON_ONCE(debug_locks &&
p->lockdep_depth)` which fires because `lockdep_depth > 0`, and then executes
`WRITE_ONCE(p->__state, TASK_FROZEN)`, overwriting the task's state while it is
still mid-`__schedule()` on another CPU.

## Consequence

The consequences of this race are twofold:

**1. lockdep WARN_ON_ONCE splat:** When `__set_task_frozen()` is called without
`rq->lock` on a task that is still in `__schedule()` (between
`RCU_INIT_POINTER(rq->curr, next)` and `prepare_lock_switch()`), the task's
`lockdep_depth` is still positive (it holds `rq->lock`). The
`WARN_ON_ONCE(debug_locks && p->lockdep_depth)` check in `__set_task_frozen()`
fires, producing a kernel warning visible in dmesg. This was the symptom
reported by Ville Syrjälä. The warning indicates it is "dangerous to freeze with
locks held" — the frozen task appears to hold `rq->lock`, which could lead to
deadlocks if the freezer proceeds.

**2. Task state corruption:** After the warning, `__set_task_frozen()` executes
`WRITE_ONCE(p->__state, TASK_FROZEN)`, overwriting the task's state to
`TASK_FROZEN` while the task is physically still executing `__schedule()` on
CPU1. This can cause the task to become permanently stuck: after the context
switch completes on CPU1, the task's state is `TASK_FROZEN` (the freezer thinks
it's frozen), but it was never properly frozen — it was in the middle of going to
sleep for a completely different reason. Depending on timing, this could lead to
a task that appears frozen but never gets thawed properly, or a task whose sleep
state is clobbered, leading to missed wakeups.

**3. General `task_call_func()` unsafety:** Beyond `__set_task_frozen()`, any
callback invoked via `task_call_func()` during this race window operates on a
task that is neither truly blocked nor safely pinned. The callback may observe
stale or transitional scheduler state (e.g., `task_curr()` returning misleading
values, incomplete dequeue state). This could cause incorrect decisions by
livepatch or RCU tasks trace code, though in practice those callers were less
sensitive to this particular edge case.

## Fix Summary

The fix refactors the rq-lock decision logic from `task_call_func()` into a new
helper function `__task_needs_rq_lock()` and adds a critical
`smp_cond_load_acquire(&p->on_cpu, !VAL)` spin-wait when the decision is that
`rq->lock` is not needed.

The new `__task_needs_rq_lock()` function performs the following ordered checks:

1. Read `p->__state`. If `TASK_RUNNING` or `TASK_WAKING`, return `true` (needs
   rq->lock).
2. `smp_rmb()`, then read `p->on_rq`. If on_rq is set, return `true` (needs
   rq->lock).
3. **New:** `smp_rmb()` followed by `smp_cond_load_acquire(&p->on_cpu, !VAL)` —
   spin-wait until `p->on_cpu` becomes 0. This guarantees the task has completed
   `finish_task()` in `finish_task_switch()` and is no longer referenced by the
   scheduler on any CPU.
4. Return `false` (rq->lock not needed; task is truly quiescent).

The `smp_cond_load_acquire()` call in step 3 mirrors the identical pattern used
in `try_to_wake_up()` for the same reason: ensuring a task is fully done with
`__schedule()` before operating on it. The acquire semantics pair with the
`smp_store_release(&prev->on_cpu, 0)` in `finish_task()`, establishing a
happens-before relationship that guarantees all scheduler state updates from the
context switch are visible.

After the fix, when `task_call_func()` proceeds without `rq->lock`, the task is
guaranteed to have `on_cpu == 0`, meaning:
- The context switch has fully completed
- `rq->lock` has been released
- `lockdep_depth` is 0 for the previous task
- `task_curr(p)` correctly returns `false`
- The task's `__state` is stable and not being concurrently modified by
  `__schedule()`

## Triggering Conditions

The bug requires the following precise conditions to manifest:

- **Multi-CPU system (>= 2 CPUs):** CPU0 calls `task_call_func()`, CPU1 runs
  `__schedule()` for the target task. This is an SMP-only bug (the
  `smp_cond_load_acquire` fix is guarded by `#ifdef CONFIG_SMP`).

- **Kernel version v6.1-rc1 through v6.1-rc5:** The bug was introduced by
  `f5d39b020809` (freezer rewrite using `task_call_func`) and fixed in
  `91dabf33ae5d`. Prior kernels don't have the `task_call_func`-based freezer.

- **A task transitioning from running to sleeping:** The target task must be
  going through `__schedule()` with a voluntary sleep (e.g.,
  `TASK_INTERRUPTIBLE`, `TASK_UNINTERRUPTIBLE`, `TASK_FREEZABLE`). The task's
  `__state` must be a sleep state and `deactivate_task()` must have set
  `on_rq = 0`, while `on_cpu` is still 1.

- **Concurrent call to `task_call_func()` from another CPU:** The call must
  happen during the window from `deactivate_task()` (step 3) to
  `finish_task(prev)` (step 7a) in `__schedule()`. This window spans
  `pick_next_task()`, the `rq->curr` update, `context_switch()` including MM
  switching, lock handoff, the actual `switch_to()`, and the beginning of
  `finish_task_switch()` on the new task. While called "very narrow" in the
  commit message, this window actually covers substantial code (potentially
  microseconds of execution time), making it hittable under load.

- **For the lockdep splat specifically:** The `WARN_ON_ONCE` in
  `__set_task_frozen()` fires only when `CONFIG_LOCKDEP` is enabled, the task's
  `lockdep_depth > 0`, and `debug_locks` is true. Additionally,
  `__set_task_frozen()` must pass the `task_curr(p)` check (returns false),
  which requires the call to happen after `RCU_INIT_POINTER(rq->curr, next)` but
  before `prepare_lock_switch()` (which decrements `lockdep_depth`). This is a
  smaller sub-window within the overall race window.

- **For `__set_task_frozen()` specifically:** The task must have `TASK_FREEZABLE`
  in its state (or be `TASK_STOPPED`/`TASK_TRACED`). The `freezer_active` static
  key must be enabled, and `pm_freezing` must be true.

- **For the general `task_call_func()` race:** Any caller of `task_call_func()`
  (freeze_task, livepatch, RCU tasks) can hit the race. The callback receives
  a task in an inconsistent, mid-switch state.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP by directly importing `task_call_func()`
via `KSYM_IMPORT` and calling it with a custom detection callback while a kthread
transitions through `__schedule()`. The core approach is statistical — repeatedly
trigger the race window and check for the inconsistent state — but the detection
mechanism is deterministic: on the buggy kernel, the callback can observe
`p->on_cpu == 1` for a sleeping, dequeued task; on the fixed kernel, this is
impossible because `__task_needs_rq_lock()` spins until `on_cpu == 0`.

### Setup

1. **QEMU configuration:** At least 2 CPUs (e.g., `qemu ... -smp 2`). CPU 0
   runs the driver, CPU 1 runs the target kthread.

2. **Import symbols:** Use `KSYM_IMPORT(task_call_func)` to import the function.
   The type signature is
   `int (*)(struct task_struct *, int (*)(struct task_struct *, void *), void *)`.
   Also use `KSYM_IMPORT_TYPED` if needed for the function pointer type.

3. **Create a kthread on CPU1:** Use `kstep_kthread_create("racer")` and bind
   it to CPU1 with `kstep_kthread_bind(p, cpumask_of(1))`, then start it with
   `kstep_kthread_start(p)`. Let it spin initially.

### Detection Callback

Write a callback function compatible with `task_call_f`:

```c
struct race_result {
    int detected;
    int on_cpu_val;
    unsigned int state_val;
    int on_rq_val;
};

static int detect_race_cb(struct task_struct *p, void *arg)
{
    struct race_result *res = (struct race_result *)arg;
    unsigned int state = READ_ONCE(p->__state);

    res->state_val = state;
    res->on_rq_val = p->on_rq;
    res->on_cpu_val = p->on_cpu;

    /*
     * If the task is in a sleep state, not on the runqueue, but still
     * on_cpu, then task_call_func() called us without rq->lock and
     * without waiting for on_cpu to clear — the race is hit.
     *
     * On the fixed kernel, __task_needs_rq_lock() spins on
     * smp_cond_load_acquire(&p->on_cpu, !VAL) before returning false,
     * so on_cpu will always be 0 when we're called without rq->lock.
     *
     * On the buggy kernel, task_call_func() skips rq->lock when
     * state != RUNNING/WAKING && on_rq == 0, without checking on_cpu.
     */
    if (state != TASK_RUNNING && state != TASK_WAKING &&
        !p->on_rq && p->on_cpu) {
        res->detected = 1;
    } else {
        res->detected = 0;
    }
    return 0;
}
```

### Race Trigger Sequence

The driver's `run()` function should:

1. **Signal the kthread to block:** Call `kstep_kthread_block(p)` which sets the
   kthread's action to `do_block_on_wq`. The kthread will pick this up and call
   `wait_event()` which internally calls `__schedule()`.

2. **Immediately enter a tight polling loop on CPU0:** After signaling the block
   action, busy-loop calling `task_call_func(p, detect_race_cb, &result)`.
   Each call either:
   - Sees the task as TASK_RUNNING/on_rq → acquires rq->lock → callback sees
     normal running state → no race (task hasn't entered sleep yet)
   - Sees the task as sleeping + on_rq=0 → on the buggy kernel, calls callback
     immediately (might catch on_cpu=1); on the fixed kernel, waits for
     on_cpu=0 first
   - Sees the task as sleeping + on_rq=0 + on_cpu=0 → task fully switched
     away, no race

3. **Check result after each call:** If `result.detected == 1`, the race is
   confirmed → `kstep_fail(...)`.

4. **Wake the kthread and repeat:** After the task has fully blocked (detected
   by seeing on_rq=0, on_cpu=0 in consecutive calls), wake it up to create
   another scheduling transition. Use the kthread's wait queue to wake it
   (set `wq_ready = 1` and `wake_up(&kt->wq)`), let it spin briefly, then
   signal another block. Repeat for many iterations (e.g., 10,000+) to
   maximize the chance of hitting the race window.

5. **Alternative approach — use `kstep_freeze_task()`:** Since kSTEP already
   provides `kstep_freeze_task()` which calls `freeze_task()` →
   `task_call_func(p, __set_task_frozen, ...)`, this is the exact code path
   that triggered the bug in practice. However, `__set_task_frozen()` requires
   the task to be in a `TASK_FREEZABLE` state. To use this approach, modify
   the kthread's blocking to use `TASK_INTERRUPTIBLE | TASK_FREEZABLE` instead
   of plain `TASK_UNINTERRUPTIBLE`. This can be done by writing a custom kthread
   that calls `set_current_state(TASK_INTERRUPTIBLE | TASK_FREEZABLE)` followed
   by `schedule()`. Then call `kstep_freeze_task(p)` during the transition.
   On the buggy kernel with `CONFIG_LOCKDEP`, the `WARN_ON_ONCE` in
   `__set_task_frozen()` should fire.

### Pass/Fail Criteria

- **Buggy kernel (v6.1-rc5):** After sufficient iterations, the callback should
  observe at least one instance where `on_cpu == 1` while state is a sleep state
  and `on_rq == 0`. Report `kstep_fail("race detected: state=%x on_rq=%d on_cpu=%d", ...)`.
  If using `kstep_freeze_task()` with `TASK_FREEZABLE`, a `WARN_ON_ONCE` should
  appear in the kernel log.

- **Fixed kernel (v6.1-rc6):** The callback should never observe `on_cpu == 1`
  when called for a sleeping, dequeued task, because `__task_needs_rq_lock()`
  spins until `on_cpu == 0`. After all iterations complete without detecting the
  race, report `kstep_pass("no race detected in N iterations")`.

### kSTEP Extensions Required

- **None mandatory.** The existing kSTEP APIs (`kstep_kthread_create/bind/start/block`,
  `KSYM_IMPORT`, access to `task_struct` fields via `internal.h`) are sufficient.
  `task_call_func` can be imported via `KSYM_IMPORT`.

- **Optional enhancement:** For the `kstep_freeze_task()` approach, the kthread
  blocking mechanism could be extended to support `TASK_FREEZABLE` states. This
  would involve adding a variant of `kstep_kthread_block()` that uses
  `TASK_INTERRUPTIBLE | TASK_FREEZABLE` when waiting, or exposing a
  `kstep_kthread_block_freezable()` API. Alternatively, the driver can manage
  its own kthread function directly.

### Expected Timing Characteristics

The race window spans from `deactivate_task()` to `finish_task()` in
`finish_task_switch()`, which includes `pick_next_task()`, the `rq->curr` update,
the entire `context_switch()` function (MM switching, lock handoff, `switch_to`),
and the beginning of `finish_task_switch()` on the new task's context. This is a
non-trivial amount of code — likely several hundred nanoseconds to low
microseconds — making the window hittable with a tight polling loop from CPU0.
With 10,000+ block/wake cycles and tight `task_call_func()` polling during each
transition, the race should be reproducible with high probability on the buggy
kernel.
