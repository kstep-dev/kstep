# Core: balance_push() vs __sched_setscheduler() Race Causes CPU Hotplug Hang

**Commit:** `04193d590b390ec7a0592630f46d559ec6564ba1`
**Affected files:** kernel/sched/core.c, kernel/sched/sched.h
**Fixed in:** v5.19-rc3
**Buggy since:** v5.11-rc1 (introduced by commit ae7927023243 "sched: Optimize finish_lock_switch()")

## Bug Description

A race condition exists between the CPU hotplug-off flow and `__sched_setscheduler()` that causes the CPU being taken offline to hang indefinitely. During CPU hotplug off, a special `balance_push_callback` is installed on the dying CPU's runqueue (`rq->balance_callback`). This callback ensures that every time `__schedule()` runs on that CPU (via `finish_lock_switch()` → `__balance_callbacks()`), the `balance_push()` function executes to push non-per-CPU-kthread tasks off the dying CPU and eventually wake up the hotplug control thread (`cpuhp/N`) via `rcuwait_wake_up(&rq->hotplug_wait)`.

The problem arises because `__sched_setscheduler()`, which can be called from any CPU to change the scheduling policy of a task on the dying CPU, uses `splice_balance_callbacks(rq)` to temporarily remove all balance callbacks from the target runqueue across a lock-break. Between the splice (which sets `rq->balance_callback = NULL`) and the later `balance_callbacks(rq, head)` call that runs them, the dying CPU can do a `schedule()` → `finish_lock_switch()` → `__balance_callbacks()` cycle and observe the empty callback list, missing the critical `balance_push()` call entirely.

When `balance_push()` is missed, the idle task on the dying CPU (swapper/N) cannot call `rcuwait_wake_up()` to wake the hotplug control thread. The CPU then remains stuck in the `rcuwait_wait_event()` loop inside `balance_hotplug_wait()`, blocking the entire hotplug-off sequence permanently. This was reported by Jing-Ting Wu at MediaTek, where the syndrome was observed as a system hang during CPU offlining that occurred within 48 hours of stress testing.

## Root Cause

The root cause lies in the interaction between two uses of the balance callback infrastructure: the CPU hotplug `balance_push_callback` (which must persistently remain on the dying CPU's callback list) and the `splice_balance_callbacks()` function used by `__sched_setscheduler()` (which unconditionally removes all callbacks across a lock-break window).

Commit ae7927023243 ("sched: Optimize finish_lock_switch()") restructured the balance callback mechanism to use a linked-list-based `rq->balance_callback` pointer instead of the previous `rq->balance_flags` bitfield approach. The new design placed `balance_push()` as a regular entry on the callback list via the global `balance_push_callback` structure. The `balance_push()` function re-installs itself at the start of each invocation (`rq->balance_callback = &balance_push_callback`) to ensure persistence. However, this persistence guarantee can be broken by an external `splice_balance_callbacks()` call.

The specific race sequence is:

1. **CPU_A** (going offline) is in state `CPUHP_AP_SCHED_WAIT_EMPTY`. During `sched_cpu_deactivate()`, it set `rq_A->balance_callback = &balance_push_callback`. CPU_A is now in `balance_hotplug_wait()` → `rcuwait_wait_event()`, looping through `schedule()` calls. Each `schedule()` → `finish_lock_switch()` → `__balance_callbacks(rq_A)` call runs `balance_push()`, which pushes tasks away and re-installs itself on the list. Eventually, CPU_A releases `rq_A->lock`.

2. **CPU_B** calls `__sched_setscheduler(p)` for a task `p` whose `task_rq(p)` is `rq_A`. It acquires `rq_A->lock` via `task_rq_lock()`, performs the scheduling class change, then calls `splice_balance_callbacks(rq_A)` which sets `rq_A->balance_callback = NULL` and saves the old head pointer to `head`. CPU_B then releases `rq_A->lock` via `task_rq_unlock()`.

3. **CPU_A** acquires `rq_A->lock` again in its `schedule()` loop. It enters `finish_lock_switch()` → `__balance_callbacks(rq_A)` → `splice_balance_callbacks(rq_A)`, but now `rq_A->balance_callback == NULL`. So `balance_push()` never runs, and `rcuwait_wake_up()` is never called.

4. **CPU_B** eventually calls `balance_callbacks(rq_A, head)`, which does run `balance_push(rq_A)`. However, `balance_push()` checks `rq != this_rq()` — since CPU_B is running this on its own CPU but with `rq_A` as the argument, and `rq_A != rq_B`, the early return path prevents the actual push and `rcuwait_wake_up()` call from executing. Meanwhile, `balance_push()` re-installs itself (`rq->balance_callback = &balance_push_callback`), but this writes to `rq_A` which CPU_A has already passed through its `__balance_callbacks()`.

5. **CPU_A** is now stuck: it keeps looping in `rcuwait_wait_event()` but each `schedule()` cycle either sees the callback already stolen again or a window where it's missing. The hotplug control thread (`cpuhp/A`) is never woken, so the CPU offline process hangs permanently.

## Consequence

The observable impact is a **permanent system hang during CPU hotplug off**. When the race triggers, the CPU being taken offline gets stuck in an infinite loop inside `balance_hotplug_wait()` → `rcuwait_wait_event()`. The CPU becomes unresponsive — it is logically in the idle loop calling `schedule()` repeatedly but never making forward progress because `balance_push()` never executes to wake the hotplug control thread.

This manifests as a system hang that is only detectable through CPU online/offline operations. Any system that performs dynamic CPU power management (e.g., mobile devices using CPU hotplug for power savings, cloud VMs with CPU hot-add/remove, or any system using `echo 0 > /sys/devices/system/cpu/cpuN/online`) is affected. The MediaTek team reported the issue occurred on ARM platforms within 48 hours of stability testing, with the trigger being concurrent scheduling policy changes (e.g., via `sched_setscheduler()` syscall) to tasks on a CPU being taken offline.

Since the hang is permanent and affects the CPU hotplug state machine, it can cascade: subsequent attempts to online/offline other CPUs may also fail or hang, eventually rendering the system unusable. There is no recovery short of a hard reboot. The bug does not cause data corruption or a kernel panic, but the indefinite hang is equally severe in production environments.

## Fix Summary

The fix introduces a `split` parameter to the `splice_balance_callbacks()` function, creating a two-mode operation via `__splice_balance_callbacks(rq, bool split)`:

1. **`split = true`** (used by `splice_balance_callbacks()`, called from `__sched_setscheduler()`): When the only callback on the list is `balance_push_callback` (i.e., `head == &balance_push_callback`), it returns `NULL` without removing the callback from the list. This ensures that during the lock-break window in `__sched_setscheduler()`, the `balance_push_callback` remains installed on the runqueue, so any interleaving `__schedule()` on the dying CPU will still see and execute it. If there are other callbacks besides `balance_push_callback`, they are all spliced off normally.

2. **`split = false`** (used by `__balance_callbacks()`, called from `finish_lock_switch()`): All callbacks including `balance_push_callback` are taken off the list and executed. This is safe because `__balance_callbacks()` is called within the same `rq->lock` section as `__schedule()`, so there is no lock-break window where the list could be observed as empty by another `__schedule()` invocation.

The fix also adds a detailed comment block above the `balance_push_callback` declaration explaining that it operates by "significantly different rules" than normal balance callbacks — specifically, it targets `__schedule()` context and must remain on the list at all times during CPU hotplug off. Additionally, the `queue_balance_callback()` function in `sched.h` receives an expanded comment documenting the existing guard (`rq->balance_callback == &balance_push_callback`) that prevents other callbacks from being queued when `balance_push` is active. This fix is minimal, correct, and complete: it preserves the persistence invariant of `balance_push_callback` across all code paths that manipulate the callback list.

## Triggering Conditions

The following precise conditions are required to trigger this bug:

- **CPU Hotplug**: A CPU must be in the process of going offline. Specifically, it must have passed through `sched_cpu_deactivate()` (which sets `rq->balance_callback = &balance_push_callback` via `balance_push_set(cpu, true)`) and be in the `CPUHP_AP_SCHED_WAIT_EMPTY` hotplug state executing `balance_hotplug_wait()` → `rcuwait_wait_event()`.

- **Concurrent sched_setscheduler**: Another CPU must call `__sched_setscheduler()` for a task whose runqueue is the dying CPU's runqueue. This happens when any thread calls `sched_setscheduler()`, `sched_setattr()`, or `rt_mutex_setprio()` for a task currently enqueued on the dying CPU. The call must be made from a different CPU than the one going offline.

- **Race window timing**: CPU_B's `splice_balance_callbacks(rq_A)` must execute after CPU_A has released `rq_A->lock` from a `finish_lock_switch()` cycle but before CPU_A re-acquires the lock for its next `schedule()` iteration. Then CPU_B must release `rq_A->lock` (via `task_rq_unlock()`) before CPU_A's next `finish_lock_switch()` → `__balance_callbacks()` call, so CPU_A observes `rq_A->balance_callback == NULL`.

- **SMP required**: The system must have at least 2 CPUs (CONFIG_SMP=y). The bug cannot occur on UP systems since there is no concurrent access to the runqueue. The reporter observed this on ARM SMP platforms.

- **Reliability**: The race window is narrow (the time between `splice_balance_callbacks()` setting the callback to NULL and `task_rq_unlock()` releasing the lock is very brief). The MediaTek team reported it takes less than 48 hours to reproduce under stability testing but is not deterministic. Systems that frequently hotplug CPUs and concurrently change task scheduling policies are most susceptible.

## Reproduce Strategy (kSTEP)

### Why This Bug Cannot Be Reproduced with kSTEP

1. **Requires CPU hotplug, which is fundamentally outside kSTEP's architecture.** The bug's triggering condition is a CPU going through the hotplug offline state machine, specifically reaching the `CPUHP_AP_SCHED_WAIT_EMPTY` state where `balance_push_callback` is active and the CPU is looping in `rcuwait_wait_event()`. kSTEP has no API or mechanism to initiate CPU hotplug events. The kSTEP topology primitives (`kstep_topo_init()`, `kstep_topo_apply()`, etc.) configure the initial topology structure but do not support dynamic CPU online/offline transitions.

2. **CPU hotplug involves the full kernel hotplug state machine, not just scheduler state.** Taking a CPU offline involves dozens of hotplug states and notifiers (`CPUHP_AP_ACTIVE`, `CPUHP_AP_SCHED_WAIT_EMPTY`, etc.), workqueue draining, RCU synchronization, interrupt migration, and other subsystem teardown. This cannot be simulated by manipulating scheduler-internal state; it requires actually calling `cpu_down()` or writing to the CPU sysfs interface, triggering the full kernel hotplug machinery.

3. **The race requires precise inter-CPU timing that is inherently non-deterministic.** Even if CPU hotplug were available, reproducing this race requires CPU_B's `__sched_setscheduler()` to execute during a very narrow window on CPU_A's hotplug flow. The race window is the brief period between CPU_A releasing `rq->lock` after `finish_lock_switch()` and CPU_A's next `schedule()` iteration acquiring the lock. kSTEP is designed for deterministic reproducers, but this bug's occurrence is probabilistic and timing-dependent.

4. **`__sched_setscheduler()` is triggered via syscall, which kSTEP cannot intercept.** kSTEP controls tasks via kernel-level APIs (`kstep_task_create`, `kstep_task_set_prio`) but the race requires `__sched_setscheduler()` specifically, which internally calls `splice_balance_callbacks()`. While `kstep_task_set_prio()` might eventually trigger `__sched_setscheduler()`, there is no guarantee that the exact code path including `splice_balance_callbacks()` is taken, and no way to ensure the timing aligns with the hotplug flow.

### What Would Need to Be Added to kSTEP

To reproduce this bug, kSTEP would need **fundamental** changes:

- **CPU hotplug support**: A `kstep_cpu_offline(cpu)` / `kstep_cpu_online(cpu)` API that calls the kernel's `cpu_down()` / `cpu_up()` functions. This is not a minor wrapper — it involves the CPU actually going through the full hotplug state machine, which has complex interactions with many kernel subsystems and takes significant time to complete.

- **Concurrent cross-CPU operations with precise timing control**: The ability to execute `sched_setscheduler()` on a remote CPU's task at a specific point during the hotplug flow. This would require either instrumentation of the hotplug state machine or a way to inject operations at specific scheduler lock acquisition points.

- **Non-deterministic race reproduction framework**: Since the bug depends on a specific interleaving of lock acquisitions across CPUs, kSTEP would need a mechanism for probabilistic race reproduction (e.g., repeated attempts with random delays) rather than its current deterministic approach.

### Alternative Reproduction Methods Outside kSTEP

The bug can be reproduced on a real (or QEMU) multi-CPU Linux system by:

1. Creating a stress test that repeatedly hotplugs CPUs offline and online (e.g., `echo 0 > /sys/devices/system/cpu/cpu1/online`).
2. Simultaneously running a workload that frequently calls `sched_setscheduler()` to change task scheduling policies for tasks that may be running on the CPU being hotplugged.
3. Running this for an extended period (the reporter achieved reproduction within 48 hours). Using `chrt` or a custom program that repeatedly calls `sched_setscheduler()` on tasks pinned to the CPU being hotplugged would increase the hit rate.
4. Detection: the system will hang when the bug triggers, with the dying CPU stuck in `rcuwait_wait_event()` and the `cpuhp/N` kthread never being woken.
