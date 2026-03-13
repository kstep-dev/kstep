# Uclamp: Deadlock When Enabling uclamp Static Key

**Commit:** `e65855a52b479f98674998cb23b21ef5a8144b04`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.9-rc1
**Buggy since:** v5.8-rc4 (introduced by commit `46609ce22703` "sched/uclamp: Protect uclamp fast path code with static key")

## Bug Description

When a user sets a uclamp value on a task via `sched_setattr()`, the kernel
enables a static branch (`sched_uclamp_used`) that gates the uclamp fast-path
code. Commit `46609ce22703` introduced this static key to avoid performance
regressions on systems where uclamp is compiled in but never used.

The problem is that `static_branch_enable()` was called from within
`__setscheduler_uclamp()`, which executes while holding the per-task `pi_lock`
and the per-CPU `rq->lock` (acquired together via `task_rq_lock()` in
`__sched_setscheduler()`). The `static_branch_enable()` function internally
calls `cpus_read_lock()`, which acquires a percpu-rwsem and is a potentially
sleeping operation. Calling a sleeping function while holding spinlocks
(specifically, raw_spinlocks in the scheduler) is illegal and triggers a
`BUG: sleeping function called from invalid context` splat.

This is a classic locking inversion / context violation bug in the scheduler.
The static key enabling mechanism requires process context with no spinlocks
held, but the scheduler's `__sched_setscheduler()` path holds multiple
spinlocks when it reaches the uclamp configuration code.

## Root Cause

The call chain that triggers the bug is:

1. Userspace calls `sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP` flags set.
2. `__sched_setscheduler()` in `kernel/sched/core.c` is invoked.
3. `__sched_setscheduler()` acquires `task_rq_lock(p, &rf)`, which takes both
   `p->pi_lock` (a raw_spinlock) and `rq->lock` (another raw_spinlock).
4. While holding these locks, it calls `__setscheduler_uclamp(p, attr)`.
5. `__setscheduler_uclamp()` checks if `SCHED_FLAG_UTIL_CLAMP` is set, and if
   so, calls `static_branch_enable(&sched_uclamp_used)`.
6. `static_branch_enable()` calls `cpus_read_lock()` (a sleeping percpu-rwsem
   read lock), which calls `percpu_down_read()`.
7. `percpu_down_read()` may sleep, violating the atomic context established by
   the held spinlocks.

The specific code path in the buggy `__setscheduler_uclamp()`:

```c
static void __setscheduler_uclamp(struct task_struct *p,
                                  const struct sched_attr *attr)
{
    /* ... reset clamps on class change ... */

    if (likely(!(attr->sched_flags & SCHED_FLAG_UTIL_CLAMP)))
        return;

    static_branch_enable(&sched_uclamp_used);  /* BUG: sleeps under spinlock */

    if (attr->sched_flags & SCHED_FLAG_UTIL_CLAMP_MIN) {
        uclamp_se_set(&p->uclamp_req[UCLAMP_MIN],
                      attr->sched_util_min, true);
    }
    /* ... */
}
```

The root issue is that `static_branch_enable()` was placed inside a function
that runs in atomic/spinlock context without considering the sleeping
requirements of the static key subsystem. The jump label (static key)
infrastructure needs to modify code text on all CPUs, which requires
`stop_machine()` or at minimum `cpus_read_lock()` to ensure no CPU is in the
middle of executing the code being patched.

## Consequence

The observable impact is a kernel BUG splat with the following backtrace:

```
BUG: sleeping function called from invalid context at ./include/linux/percpu-rwsem.h:49

  cpus_read_lock+0x68/0x130
  static_key_enable+0x1c/0x38
  __sched_setscheduler+0x900/0xad8
```

This occurs every time a task's uclamp attributes are set via `sched_setattr()`
when the static key has not yet been enabled. On kernels with
`CONFIG_DEBUG_ATOMIC_SLEEP=y`, this triggers immediately as a warning/BUG. On
production kernels without that debug option, the actual consequence depends on
the percpu-rwsem implementation: it could lead to a deadlock if the rwsem needs
to actually sleep (e.g., if a writer is holding the write side), or it could
appear to work in the common uncontended case but leave the lock state
inconsistent. In the worst case, this can cause a hard deadlock of the
scheduler, making the CPU unresponsive.

The bug is triggered deterministically on the first call to `sched_setattr()`
with uclamp flags on a kernel where `sched_uclamp_used` has not yet been
enabled. Subsequent calls may not trigger it because `static_branch_enable()`
is a no-op when the key is already enabled.

## Fix Summary

The fix moves the `static_branch_enable(&sched_uclamp_used)` call from
`__setscheduler_uclamp()` (which runs under `task_rq_lock()`) to
`uclamp_validate()` (which runs before the scheduler locks are acquired in
`__sched_setscheduler()`).

In `__sched_setscheduler()`, the flow is:
1. `uclamp_validate(p, attr)` — validates uclamp parameters, runs **without**
   scheduler locks held.
2. `task_rq_lock(p, &rf)` — acquires the spinlocks.
3. `__setscheduler_uclamp(p, attr)` — applies the uclamp values, runs **with**
   scheduler locks held.

By moving `static_branch_enable()` into step 1 (`uclamp_validate()`), the
sleeping operation executes in the correct process context before any scheduler
spinlocks are taken. The comment added to `uclamp_validate()` explains:

```c
/*
 * We have valid uclamp attributes; make sure uclamp is enabled.
 *
 * We need to do that here, because enabling static branches is a
 * blocking operation which obviously cannot be done while holding
 * scheduler locks.
 */
static_branch_enable(&sched_uclamp_used);
```

This is safe because `uclamp_validate()` is called unconditionally before the
lock acquisition in `__sched_setscheduler()`, and enabling an already-enabled
static key is a no-op. The fix also removes the now-redundant
`static_branch_enable()` line from `__setscheduler_uclamp()`.

## Triggering Conditions

- **Kernel version:** The bug exists only in kernels between v5.8-rc4 (when
  commit `46609ce22703` was merged) and v5.9-rc1 (when this fix was merged).
  Essentially, the bug is present in the v5.8 release series only.
- **Configuration:** `CONFIG_UCLAMP_TASK=y` must be enabled. Additionally,
  `CONFIG_DEBUG_ATOMIC_SLEEP=y` is needed to see the BUG splat (though the
  underlying issue exists regardless).
- **Trigger action:** A process must call `sched_setattr()` with
  `SCHED_FLAG_UTIL_CLAMP_MIN` or `SCHED_FLAG_UTIL_CLAMP_MAX` set in
  `sched_flags`, when the `sched_uclamp_used` static key has not yet been
  enabled. This is typically the **first** such call after boot.
- **No special topology or CPU count requirements.** The bug is triggered on
  any system (single CPU or multi-CPU) the first time uclamp attributes are set
  via the syscall path.
- **Deterministic:** The bug is 100% reproducible on the first
  `sched_setattr()` call with uclamp flags. No race condition or timing
  sensitivity is involved — it is a straightforward context violation.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Kernel version too old (pre-v5.15).** The bug was introduced in v5.8-rc4
   by commit `46609ce22703` and fixed in v5.9-rc1 by commit `e65855a52b47`.
   kSTEP supports Linux v5.15 and newer only. By v5.15, this bug has long been
   fixed, so there is no buggy kernel available within kSTEP's supported range.

2. **Requires `sched_setattr()` syscall from userspace.** Even if the kernel
   version were supported, triggering this bug requires a userspace process to
   invoke the `sched_setattr()` syscall with `SCHED_FLAG_UTIL_CLAMP` flags.
   kSTEP operates via a kernel module and controls tasks through kernel APIs
   (`kstep_task_create()`, `kstep_task_set_prio()`, etc.). There is no kSTEP
   API to set uclamp attributes on a task — `kstep_task_set_prio()` goes
   through `sched_setscheduler()` which does not set uclamp flags. kSTEP cannot
   intercept or issue userspace syscalls.

3. **Nature of the bug is a locking context violation, not a scheduling logic
   error.** The bug manifests as a `BUG: sleeping function called from invalid
   context` diagnostic. It does not produce incorrect scheduling behavior that
   could be observed through task placement, vruntime values, or other
   scheduler state. kSTEP's observation APIs (`kstep_eligible()`,
   `kstep_output_curr_task()`, etc.) are designed to detect scheduling logic
   errors, not locking violations.

4. **What would need to change in kSTEP:** To support this class of bug, kSTEP
   would need: (a) a `kstep_task_set_uclamp(p, min, max)` API that internally
   calls `sched_setattr()` with `SCHED_FLAG_UTIL_CLAMP` flags, and (b) support
   for kernels older than v5.15. The first change is a minor API addition, but
   the second is a fundamental limitation.

5. **Alternative reproduction methods:** The simplest way to reproduce this bug
   outside kSTEP is to boot a v5.8 kernel (with `CONFIG_UCLAMP_TASK=y` and
   `CONFIG_DEBUG_ATOMIC_SLEEP=y`) and run a simple userspace program that calls:
   ```c
   struct sched_attr attr = {
       .size = sizeof(attr),
       .sched_flags = SCHED_FLAG_UTIL_CLAMP_MIN,
       .sched_util_min = 512,
   };
   syscall(__NR_sched_setattr, 0, &attr, 0);
   ```
   The BUG splat will appear immediately in `dmesg`.
