# Deadline: Warning in migrate_enable for PI-Boosted Tasks

**Commit:** `0664e2c311b9fa43b33e3e81429cd0c2d7f9c638`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.13-rc3
**Buggy since:** v4.15-rc1 (introduced by commit `295d6d5e3736` "sched/deadline: Fix switching to -deadline")

## Bug Description

When a SCHED_DEADLINE task is priority-inherited (PI-boosted) via an rt_mutex and subsequently has its CPU affinity changed (e.g., through `migrate_enable` on PREEMPT_RT or an explicit `set_cpus_allowed_ptr` call), the kernel triggers a `WARN_ON` inside `setup_new_dl_entity()`. This occurs because the affinity change path dequeues the task with `DEQUEUE_SAVE` and re-enqueues it with `ENQUEUE_RESTORE`, and the `ENQUEUE_RESTORE` path in `enqueue_dl_entity()` unconditionally calls `setup_new_dl_entity()` when the task's deadline is in the past — without first checking whether the task is PI-boosted.

The function `setup_new_dl_entity()` contains an explicit `WARN_ON(is_dl_boosted(dl_se))` guard at line 807 of deadline.c (in the buggy version), because a PI-boosted task already had its scheduling parameters (deadline, runtime) configured by `rt_mutex_setprio()` during the PI inheritance chain. Reinitializing those parameters via `setup_new_dl_entity()` is both unnecessary and incorrect — it would overwrite the inherited scheduling parameters that are actively being used for priority inheritance.

The bug was originally reported when running `stress-ng --cyclic` in a loop, which creates many SCHED_DEADLINE tasks that interact with signals (kill syscall). On PREEMPT_RT kernels, the `group_send_sig_info()` path acquires an `rt_spin_lock`, which is an rt_mutex under PREEMPT_RT. When unlocking (`rt_spin_unlock`), `migrate_enable()` is called, which may trigger `__set_cpus_allowed_ptr` → `__do_set_cpus_allowed` on the current task. If that task happens to be PI-boosted by a SCHED_DEADLINE task and its own deadline has expired, the warning fires.

## Root Cause

The root cause lies in the `enqueue_dl_entity()` function in `kernel/sched/deadline.c`. This function handles the re-enqueue of deadline scheduling entities and has three main branches for parameter initialization:

```c
if (flags & ENQUEUE_WAKEUP) {
    task_contending(dl_se, flags);
    update_dl_entity(dl_se);
} else if (flags & ENQUEUE_REPLENISH) {
    replenish_dl_entity(dl_se);
} else if ((flags & ENQUEUE_RESTORE) &&
           dl_time_before(dl_se->deadline, rq_clock(rq_of_dl_se(dl_se)))) {
    setup_new_dl_entity(dl_se);
}
```

The third branch handles the `ENQUEUE_RESTORE` case, which is used by `__do_set_cpus_allowed()` when changing a task's CPU affinity. This path dequeues the task with `DEQUEUE_SAVE` and re-enqueues with `ENQUEUE_RESTORE`. When the task's deadline is in the past (`dl_time_before(dl_se->deadline, rq_clock(...))`), the code calls `setup_new_dl_entity()` to reinitialize the deadline parameters.

The problem is that this branch does not check whether the task is PI-boosted before calling `setup_new_dl_entity()`. The function `setup_new_dl_entity()` explicitly forbids being called on boosted tasks:

```c
static inline void setup_new_dl_entity(struct sched_dl_entity *dl_se)
{
    struct dl_rq *dl_rq = dl_rq_of_se(dl_se);
    struct rq *rq = rq_of_dl_rq(dl_rq);

    WARN_ON(is_dl_boosted(dl_se));
    WARN_ON(dl_time_before(rq_clock(rq), dl_se->deadline));
    ...
    replenish_dl_new_period(dl_se, rq);
}
```

A task is considered PI-boosted when `pi_of(dl_se) != dl_se`, meaning the task's effective deadline scheduling entity (`pi_se`) has been replaced by the blocked owner's entity through the PI chain established by `rt_mutex_setprio()`. The PI mechanism ensures the boosted task runs with the inherited priority parameters; calling `setup_new_dl_entity()` would overwrite these with freshly computed values (current time + relative deadline), breaking the PI invariant.

The specific call chain that triggers this is: `__do_set_cpus_allowed()` → `dequeue_task(rq, p, DEQUEUE_SAVE | DEQUEUE_NOCLOCK)` → ... → `enqueue_task(rq, p, ENQUEUE_RESTORE | ENQUEUE_NOCLOCK)` → `enqueue_task_dl()` → `enqueue_dl_entity(dl_se, flags)`. At this point, `flags` contains `ENQUEUE_RESTORE`, and if the deadline has expired, the buggy code path is entered without regard for PI boosting state.

Note that `enqueue_task_dl()` itself has a check for `is_dl_boosted(&p->dl)` at its top, but that check only handles the throttled-and-boosted case by clearing the throttle flag. It does not prevent the `ENQUEUE_RESTORE` path in `enqueue_dl_entity()` from calling `setup_new_dl_entity()`.

## Consequence

The immediate observable consequence is a kernel `WARN_ON` splat in `setup_new_dl_entity()` at `kernel/sched/deadline.c:807` (in the buggy version). This produces a full stack trace in dmesg/console output. The warning itself does not cause a kernel crash or panic (unless `panic_on_warn` is enabled), but it indicates a logic violation in the scheduler.

Beyond the warning, `setup_new_dl_entity()` proceeds to call `replenish_dl_new_period()`, which overwrites the task's deadline and runtime with freshly computed values based on the current wall clock time and the task's own scheduling parameters (not the inherited ones). This can corrupt the PI-inherited scheduling parameters: the boosted task may end up running with a deadline that is different from what the PI mechanism intended, potentially causing incorrect scheduling decisions. Specifically, the task could get a later deadline than the PI-inherited one, defeating the purpose of PI boosting — a higher-priority DL task blocked on the rt_mutex may experience unbounded priority inversion.

The stack trace from the report shows the bug triggering through the `rt_spin_unlock → migrate_enable → __set_cpus_allowed_ptr → __do_set_cpus_allowed → enqueue_task_dl → enqueue_dl_entity → setup_new_dl_entity` path, specifically during a `kill()` syscall (`__x64_sys_kill → kill_proc_info → kill_pid_info → group_send_sig_info → rt_spin_unlock`). On production PREEMPT_RT systems, this path can be hit frequently, leading to repeated warning splats that flood the kernel log and may trigger monitoring alerts.

## Fix Summary

The fix adds a single guard condition `!is_dl_boosted(dl_se)` to the `ENQUEUE_RESTORE` branch in `enqueue_dl_entity()`:

```c
} else if ((flags & ENQUEUE_RESTORE) &&
           !is_dl_boosted(dl_se) &&
           dl_time_before(dl_se->deadline, rq_clock(rq_of_dl_se(dl_se)))) {
    setup_new_dl_entity(dl_se);
}
```

When a PI-boosted task is re-enqueued with `ENQUEUE_RESTORE` (e.g., during a CPU affinity change), the code now skips the `setup_new_dl_entity()` call entirely. This is correct because a boosted task's scheduling parameters have already been properly set by `rt_mutex_setprio()` and must not be overwritten. The boosted task should continue running with its PI-inherited parameters until the PI chain is resolved (i.e., until the rt_mutex is released and `rt_mutex_setprio()` reverts the boosting).

The fix is minimal and surgical — exactly one line added. The author considered an alternative approach of introducing a new `ENQUEUE_SET_CPUS_ALLOWED` flag to bypass the `setup_new_dl_entity()` call specifically for the affinity change path, but this was rejected as overly complex given that the simpler `is_dl_boosted()` check achieves the same practical effect and is consistent with how other parts of the deadline scheduler handle boosted tasks (e.g., `dl_task_timer`, `update_dl_entity`, etc. all check `is_dl_boosted` before modifying deadline parameters).

## Triggering Conditions

The bug requires the following precise conditions to be met simultaneously:

1. **A SCHED_DEADLINE task exists.** The task must be running under the `SCHED_DEADLINE` scheduling policy so that it goes through the `enqueue_dl_entity()` path.

2. **The task is PI-boosted via an rt_mutex.** The task must hold an rt_mutex (or rt_spin_lock on PREEMPT_RT), and a higher-priority SCHED_DEADLINE task must be blocked waiting on that same mutex. This causes `rt_mutex_setprio()` to set `dl_se->pi_se` to the waiter's `dl_se`, making `is_dl_boosted()` return true.

3. **The task's deadline is in the past.** The task's absolute deadline (`dl_se->deadline`) must be before the current runqueue clock (`rq_clock(rq)`). This is the second condition in the `ENQUEUE_RESTORE` branch. This naturally happens when a SCHED_DEADLINE task has been running for longer than its period, or has been sleeping/blocked past its deadline.

4. **A CPU affinity change (or similar DEQUEUE_SAVE/ENQUEUE_RESTORE operation) occurs on the boosted task.** The most common trigger is `__do_set_cpus_allowed()`, which dequeues with `DEQUEUE_SAVE` and re-enqueues with `ENQUEUE_RESTORE`. On PREEMPT_RT kernels, this happens via `migrate_enable()` after an `rt_spin_unlock()`. On non-RT kernels, it can happen via explicit `sched_setaffinity()` or `set_cpus_allowed_ptr()` calls.

The bug does **not** require PREEMPT_RT — it can be triggered on any kernel configuration that has `CONFIG_SCHED_DEADLINE=y` (which is typically always enabled) and `CONFIG_RT_MUTEXES=y` (enabled by default on most distributions). The PREEMPT_RT angle in the original report is simply the most common production trigger path because rt_spin_locks (which are rt_mutexes under PREEMPT_RT) are ubiquitous.

For reproduction, at least 2 CPUs are needed (so the affinity change is meaningful), and the workload must create contention between SCHED_DEADLINE tasks on rt_mutexes while also triggering affinity changes. The `stress-ng --cyclic` workload achieves this by creating many SCHED_DEADLINE threads that interact via signals, but a more targeted approach using rt_mutexes directly is more reliable.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP with minor extensions. The core requirement is to create a PI-boosted SCHED_DEADLINE task and then trigger a `set_cpus_allowed_ptr` call on it while its deadline has expired.

### Required kSTEP Extensions

1. **SCHED_DEADLINE task support:** kSTEP currently has `kstep_task_fifo()` and `kstep_task_cfs()` but no SCHED_DEADLINE equivalent. Add a new function `kstep_task_dl(p, runtime_ns, deadline_ns, period_ns)` that calls `sched_setattr_nocheck()` (imported via `KSYM_IMPORT`) to set `SCHED_DEADLINE` policy on a kthread with the specified parameters. Alternatively, the driver can use `KSYM_IMPORT(sched_setattr_nocheck)` directly and construct a `struct sched_attr`.

2. **rt_mutex PI boosting:** This can be done entirely with existing kernel APIs. The driver would import `rt_mutex_init()`, `rt_mutex_lock()`, and `rt_mutex_unlock()` (or use them directly since they're available in modules). Two kthreads contend on an rt_mutex to create PI boosting.

3. **CPU affinity change on boosted task:** Call `set_cpus_allowed_ptr()` (imported via KSYM_IMPORT) on the boosted task while it holds the mutex and has an expired deadline. This triggers the `__do_set_cpus_allowed` → `ENQUEUE_RESTORE` path.

### Step-by-Step Driver Plan

1. **Topology:** Configure QEMU with at least 2 CPUs. Use `kstep_topo_init()` with a simple 2-CPU setup.

2. **Create two kthreads:**
   - Thread A (lower priority DL): runtime=5ms, deadline=10ms, period=10ms. Pin to CPU 1.
   - Thread B (higher priority DL): runtime=5ms, deadline=8ms, period=8ms (shorter deadline = higher priority). Pin to CPU 1.

3. **Set SCHED_DEADLINE policy** on both threads using `KSYM_IMPORT(sched_setattr_nocheck)`:
   ```c
   struct sched_attr attr = {
       .sched_policy = SCHED_DEADLINE,
       .sched_runtime = 5 * NSEC_PER_MSEC,
       .sched_deadline = 10 * NSEC_PER_MSEC,
       .sched_period = 10 * NSEC_PER_MSEC,
   };
   sched_setattr_nocheck(taskA, &attr);
   ```

4. **Create an rt_mutex** in the driver:
   ```c
   static struct rt_mutex test_mutex;
   rt_mutex_init(&test_mutex);
   ```

5. **Thread A's function:** Lock the rt_mutex, then busy-loop or sleep long enough for its deadline to expire (e.g., more than one full period). While holding the mutex with an expired deadline, signal readiness to the driver.

6. **Thread B's function:** Wait for Thread A to acquire the mutex, then attempt to lock the same mutex. This blocks Thread B and causes PI boosting of Thread A (Thread A inherits Thread B's shorter deadline priority). Thread B blocks inside `rt_mutex_lock()`.

7. **Driver (on CPU 0):** After confirming that Thread A is PI-boosted (check `is_dl_boosted(&taskA->dl)` via internal access to `task_struct`), and that Thread A's deadline is in the past (`dl_time_before(taskA->dl.deadline, rq_clock(cpu_rq(1)))`), call `set_cpus_allowed_ptr(taskA, cpu_possible_mask)` or change its affinity to include both CPUs. This triggers the `__do_set_cpus_allowed` → `dequeue(DEQUEUE_SAVE)` → `enqueue(ENQUEUE_RESTORE)` path.

8. **Detection:** On the buggy kernel, `setup_new_dl_entity()` fires `WARN_ON(is_dl_boosted(dl_se))`. This can be detected by:
   - Checking dmesg for the WARN_ON message (`setup_new_dl_entity` warning at `deadline.c:807`)
   - Recording Thread A's `dl_se->deadline` before and after the `set_cpus_allowed_ptr` call — on the buggy kernel, the deadline will be overwritten by `replenish_dl_new_period()` to a fresh value (current_time + relative_deadline), while on the fixed kernel, the inherited deadline should remain unchanged.
   - Use `kstep_pass()` if the deadline remains unchanged (fixed kernel behavior) and `kstep_fail()` if it changes or if the WARN_ON is detected.

9. **Cleanup:** Have Thread A release the rt_mutex, which unblocks Thread B and resolves the PI chain. Then stop both kthreads.

### Expected Behavior

- **Buggy kernel:** The `WARN_ON(is_dl_boosted(dl_se))` in `setup_new_dl_entity()` fires, producing a kernel warning in dmesg. Additionally, Thread A's deadline is incorrectly reinitialized to `rq_clock + relative_deadline`, destroying the PI-inherited parameters.

- **Fixed kernel:** The `!is_dl_boosted(dl_se)` check in the `ENQUEUE_RESTORE` branch prevents `setup_new_dl_entity()` from being called. No warning is produced, and Thread A's PI-inherited deadline parameters are preserved intact.

### Callback Usage

The `on_tick_begin` callback can be used to monitor the state of Thread A across ticks, logging `is_dl_boosted(&taskA->dl)`, `taskA->dl.deadline`, and `taskA->dl.runtime` at each tick to provide a clear timeline of events. The main driver function orchestrates the sequence synchronously using `kstep_kthread_create`, `kstep_kthread_start`, and `kstep_sleep`/`kstep_tick_repeat` for timing.

### Implementation Notes

- Use `KSYM_IMPORT(sched_setattr_nocheck)` and `KSYM_IMPORT(set_cpus_allowed_ptr)` to access internal kernel APIs.
- Access `is_dl_boosted` via `#include "internal.h"` which gives access to `sched.h` internals.
- The driver should use `kstep_kthread_create()` for Thread A and Thread B since they need to execute kernel code (rt_mutex operations).
- Thread A must busy-wait or use a completion to ensure its deadline expires before Thread B attempts the lock.
- The `rt_mutex_lock`/`rt_mutex_unlock` APIs are exported symbols available to kernel modules.
