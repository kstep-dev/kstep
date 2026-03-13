# Core: TASK_state Bitmask Comparisons Broken by TASK_FREEZABLE Modifier

**Commit:** `5aec788aeb8eb74282b75ac1b317beb0fbb69a42`
**Affected files:** `kernel/sched/core.c`, `include/linux/wait.h`, `kernel/hung_task.c`
**Fixed in:** v6.1-rc1
**Buggy since:** v6.1-rc1 (introduced by `f5d39b020809` "freezer,sched: Rewrite core freezer logic", same merge window)

## Bug Description

The Linux kernel task state (`t->__state`) is fundamentally a bitmask composed of a base state (e.g., `TASK_INTERRUPTIBLE = 0x1`, `TASK_UNINTERRUPTIBLE = 0x2`) plus optional modifier flags (e.g., `TASK_WAKEKILL = 0x100`, `TASK_NOLOAD = 0x400`, `TASK_FREEZABLE = 0x2000`). Convenience macros combine these bits: `TASK_KILLABLE = TASK_WAKEKILL | TASK_UNINTERRUPTIBLE`, `TASK_IDLE = TASK_NOLOAD | TASK_UNINTERRUPTIBLE`.

Commit `f5d39b020809` ("freezer,sched: Rewrite core freezer logic") introduced a new state modifier `TASK_FREEZABLE` (0x2000) that can be ORed with `TASK_INTERRUPTIBLE` or `TASK_UNINTERRUPTIBLE` to mark a task as eligible for the kernel freezer (used during system suspend/hibernate). This new modifier is applied by the `wait_event_freezable()` family of macros, which pass `(TASK_INTERRUPTIBLE | TASK_FREEZABLE)` as the wait state.

However, several existing code paths throughout the kernel used direct equality comparisons (`==`) against task state values, assuming the state would be exactly one of the known composite values. With the addition of `TASK_FREEZABLE`, these comparisons break because the state now contains an additional bit that was not anticipated. For example, `state == TASK_INTERRUPTIBLE` fails when the actual state is `TASK_INTERRUPTIBLE | TASK_FREEZABLE` (0x2001), even though the task is logically interruptible.

This bug affects three distinct code paths: (1) the `___wait_is_interruptible()` macro in `include/linux/wait.h`, which determines whether a wait loop should honor pending signals; (2) the `check_hung_uninterruptible_tasks()` function in `kernel/hung_task.c`, which monitors tasks stuck in uninterruptible sleep; and (3) the `state_filter_match()` function in `kernel/sched/core.c`, which filters tasks for diagnostic display (SysRq-T). The first is the most severe, as it causes freezable interruptible waits to become effectively uninterruptible.

## Root Cause

The root cause is the use of exact equality comparisons (`==`) on task state values that are bitmasks, rather than using bitwise tests (`&`). When `TASK_FREEZABLE` was introduced as a new modifier bit, it could be ORed with existing base states, but the equality comparisons did not account for the possibility of additional bits.

**`___wait_is_interruptible()` in `include/linux/wait.h`:**
The buggy code is:
```c
#define ___wait_is_interruptible(state)                     \
    (!__builtin_constant_p(state) ||                        \
        state == TASK_INTERRUPTIBLE || state == TASK_KILLABLE)
```
When `state` is a compile-time constant (which it is for all `wait_event_*` macro invocations), this macro checks whether the state is exactly `TASK_INTERRUPTIBLE` (0x1) or exactly `TASK_KILLABLE` (0x102). But `wait_event_freezable()` passes `TASK_INTERRUPTIBLE | TASK_FREEZABLE` (0x2001), which matches neither. The macro returns false, causing the `if (___wait_is_interruptible(state) && __int)` branch in `___wait_event()` to never be taken. This means that when `prepare_to_wait_event()` detects a pending signal and returns a nonzero value, the signal is silently ignored and the wait loop continues indefinitely.

**`check_hung_uninterruptible_tasks()` in `kernel/hung_task.c`:**
The buggy code is:
```c
if (READ_ONCE(t->__state) == TASK_UNINTERRUPTIBLE)
    check_hung_task(t, timeout);
```
This only checks tasks whose state is exactly `TASK_UNINTERRUPTIBLE` (0x2). Tasks in `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE` (0x2002) are skipped, meaning the hung task detector does not monitor freezable uninterruptible sleeps. If such a task genuinely hangs (e.g., due to a deadlock), no warning is emitted.

**`state_filter_match()` in `kernel/sched/core.c`:**
The buggy code is:
```c
if (state_filter == TASK_UNINTERRUPTIBLE && state == TASK_IDLE)
    return false;
```
`TASK_IDLE` is `TASK_UNINTERRUPTIBLE | TASK_NOLOAD` (0x402). When filtering for `TASK_UNINTERRUPTIBLE`, this code correctly excludes tasks in the exact `TASK_IDLE` state. However, a task in `TASK_IDLE | TASK_FREEZABLE` (0x2402) does not match `== TASK_IDLE`, so it passes the filter and is incorrectly included in the output. The intent of the filter is to exclude any task with the `TASK_NOLOAD` bit (which signals "don't count this in load stats"), regardless of other modifier bits.

## Consequence

The most severe consequence is in the `___wait_is_interruptible()` macro. Any kernel thread or process using `wait_event_freezable()`, `wait_event_freezable_timeout()`, or `wait_event_freezable_exclusive()` becomes unable to be interrupted by signals. This effectively turns interruptible waits into uninterruptible waits. A task in such a wait cannot be killed with `SIGKILL` or any other signal — only satisfying the wait condition will release it. This was the bug reported by Christian Borntraeger: freezable interruptible waits stopped responding to signals after the freezer rewrite.

The `hung_task.c` consequence is that tasks in `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE` state are invisible to the hung task watchdog. If a filesystem or driver uses a freezable uninterruptible wait and the wait condition is never met (e.g., due to a remote server becoming unreachable), the system produces no hung task warning after the default 120-second timeout. This degrades system monitoring and makes deadlocks in freezable paths much harder to diagnose.

The `kernel/sched/core.c` consequence is cosmetic but confusing: when SysRq-T (show all tasks) is invoked with a `TASK_UNINTERRUPTIBLE` filter, tasks in `TASK_IDLE | TASK_FREEZABLE` state are incorrectly shown, polluting the diagnostic output with tasks that should be excluded by the `TASK_NOLOAD` filter.

## Fix Summary

The fix replaces all three direct equality comparisons with proper bitmask operations.

For `___wait_is_interruptible()`, the fix changes the macro to:
```c
#define ___wait_is_interruptible(state)                     \
    (!__builtin_constant_p(state) ||                        \
     (state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)))
```
This uses a bitwise AND to test whether either the `TASK_INTERRUPTIBLE` (0x1) or `TASK_WAKEKILL` (0x100) bit is set, regardless of any modifier bits like `TASK_FREEZABLE`. This correctly identifies `TASK_INTERRUPTIBLE | TASK_FREEZABLE` as interruptible (the `TASK_INTERRUPTIBLE` bit is set), and also correctly identifies `TASK_KILLABLE` (`TASK_WAKEKILL | TASK_UNINTERRUPTIBLE`) as interruptible (the `TASK_WAKEKILL` bit is set). Note that `TASK_UNINTERRUPTIBLE` alone has neither bit, so pure uninterruptible waits correctly return false.

For `check_hung_uninterruptible_tasks()`, the fix changes to:
```c
state = READ_ONCE(t->__state);
if ((state & TASK_UNINTERRUPTIBLE) && !(state & TASK_WAKEKILL))
    check_hung_task(t, timeout);
```
This checks whether the `TASK_UNINTERRUPTIBLE` bit is set (catching all variants including with `TASK_FREEZABLE`) while explicitly excluding `TASK_KILLABLE` tasks (those with `TASK_WAKEKILL` set, which can be killed and thus should not trigger hung task warnings).

For `state_filter_match()`, the fix changes to:
```c
if (state_filter == TASK_UNINTERRUPTIBLE && (state & TASK_NOLOAD))
    return false;
```
This uses a bitwise test for `TASK_NOLOAD` instead of an exact comparison with `TASK_IDLE`, correctly excluding any task with the "no load" modifier regardless of what other bits (like `TASK_FREEZABLE`) are also set.

## Triggering Conditions

**For the `___wait_is_interruptible()` bug (most impactful):**
- The kernel must be compiled with `CONFIG_FREEZER=y` (typically enabled for suspend/hibernate).
- Any code path that uses `wait_event_freezable()`, `wait_event_freezable_timeout()`, or `wait_event_freezable_exclusive()` is affected. These macros are used throughout the kernel: USB gadgets, NFS, Btrfs, various filesystem daemons, input subsystem, and more.
- A signal must be sent to the waiting task (e.g., `SIGTERM`, `SIGKILL`, `SIGINT`).
- On the buggy kernel, the signal is ignored and the task remains blocked.
- This is 100% deterministic and always reproducible on any configuration using freezable waits.

**For the `check_hung_uninterruptible_tasks()` bug:**
- The kernel must have `CONFIG_DETECT_HUNG_TASK=y`.
- A task must be in `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE` state for longer than `sysctl_hung_task_timeout_secs` (default 120 seconds).
- The hung task detector runs periodically and will skip such tasks, producing no warning.
- 100% deterministic: the hung task detector always skips these tasks.

**For the `state_filter_match()` bug in `kernel/sched/core.c`:**
- A task must be in `TASK_IDLE | TASK_FREEZABLE` (= `TASK_UNINTERRUPTIBLE | TASK_NOLOAD | TASK_FREEZABLE`) state. This requires a code path that sets `TASK_IDLE | TASK_FREEZABLE` as its wait state.
- `show_state_filter(TASK_UNINTERRUPTIBLE)` must be called (e.g., via SysRq-T or `echo t > /proc/sysrq-trigger`).
- The task would incorrectly appear in the output despite having `TASK_NOLOAD` set.
- This is 100% deterministic given the state condition, but `TASK_IDLE | TASK_FREEZABLE` as a combined state may be rare in practice.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP, for the following reasons:

**1. Why can this bug not be reproduced with kSTEP?**

The bug has three components, none of which are amenable to kSTEP reproduction:

**a) `___wait_is_interruptible()` (wait.h) — the primary behavioral bug.** This requires a kernel thread or task to enter a `wait_event_freezable()` wait (which sets state to `TASK_INTERRUPTIBLE | TASK_FREEZABLE`) and then receive a signal. kSTEP's kthread API (`kstep_kthread_create`, `kstep_kthread_block`, etc.) controls kthreads through the framework's own state machine — it does not allow kthreads to execute arbitrary code like `wait_event_freezable()`. There is no mechanism in kSTEP to make a kthread enter a specific wait queue with a specific wait state, nor to send POSIX signals to kthreads. The bug is in the `wait_event` macro expansion, which is a compile-time code generation issue in the wait queue infrastructure, not a scheduling algorithm bug.

**b) `check_hung_uninterruptible_tasks()` (hung_task.c) — outside the scheduler.** This code is in `kernel/hung_task.c`, not in `kernel/sched/`. It runs as a kernel watchdog thread that periodically iterates all tasks and checks their states. Reproducing this would require: (i) a task stuck in `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE` for 120+ seconds, (ii) the hung task detector to run and miss it, and (iii) observing the absence of a warning. kSTEP has no API to interact with the hung task detector, and the 120-second timeout is impractical in a kSTEP test that runs in accelerated kernel time.

**c) `state_filter_match()` (sched/core.c) — diagnostic display function.** This is the only component in `kernel/sched/`, but it is a diagnostic display function called by `show_state_filter()` (SysRq-T handler). It is `static inline`, so it cannot be imported with `KSYM_IMPORT`. Calling `show_state_filter()` programmatically from a module is possible (it is exported), but it writes to the kernel log, not to a programmable interface. Verifying "incorrect output" vs "correct output" would require parsing kernel log messages, which is fragile and not the intended use of kSTEP's pass/fail mechanism. Furthermore, getting a task into `TASK_IDLE | TASK_FREEZABLE` state would require either (a) the freezer subsystem actively freezing tasks, or (b) directly manipulating `t->__state`, which violates kSTEP's principle of not directly manipulating internal scheduler state.

**2. What would need to be added to kSTEP?**

Reproducing the most impactful bug (wait.h) would require fundamental additions:
- **Custom kthread code execution:** A `kstep_kthread_run_func(p, fn)` API that allows a kthread to execute an arbitrary function (like calling `wait_event_freezable()`). This is a significant architectural change since kSTEP's kthread control model is based on external state transitions, not internal code execution.
- **Signal delivery:** A `kstep_send_signal(p, sig)` API to send POSIX signals to tasks. This requires the target to have proper signal handling setup, which kernel threads may not fully support.
- **Wait queue infrastructure:** The ability to create and manage wait queues from the driver, set conditions, and verify that `wait_event_freezable()` properly returns `-ERESTARTSYS` on signal delivery.

For the `state_filter_match()` bug, you would need:
- **Freezer simulation:** The ability to put tasks into `TASK_FREEZABLE` states through the kernel's freezer API (`freeze_task()`, `thaw_process()`), which requires CONFIG_FREEZER and the PM suspend/hibernate infrastructure.
- **Diagnostic output capture:** A way to programmatically capture and verify the output of `show_state_filter()`.

These are not minor extensions — they represent fundamental additions to kSTEP's architecture (custom kthread code execution, signal delivery, freezer subsystem interaction).

**3. Version compatibility:**

The kernel version is compatible (post-v5.15; both introduction and fix are in the v6.1-rc1 cycle). Version is not the reason for being unplanned.

**4. Alternative reproduction methods:**

Outside kSTEP, this bug can be reproduced with a simple kernel module or userspace test:
- **Module approach:** Write a kernel module that creates a kthread calling `wait_event_freezable()` on a waitqueue with an always-false condition. From the module's init, after the kthread is sleeping, call `send_sig(SIGTERM, kthread_task, 0)`. On the buggy kernel, `wait_event_freezable()` never returns. On the fixed kernel, it returns `-ERESTARTSYS`.
- **Userspace approach:** Any userspace program that triggers a code path using `wait_event_freezable()` (e.g., certain USB or filesystem operations) while receiving signals would demonstrate the bug. The task would appear unkillable.
- **Hung task approach:** Create a task in `TASK_UNINTERRUPTIBLE | TASK_FREEZABLE` (e.g., via a filesystem operation on a frozen/unreachable server), wait 120+ seconds, and verify no hung task warning appears in `dmesg`.
