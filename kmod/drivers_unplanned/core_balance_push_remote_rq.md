# Core: balance_push() invoked on remote CPU during hotplug

**Commit:** `868ad33bfa3bf39960982682ad3a0f8ebda1656e`
**Affected files:** `kernel/sched/core.c`
**Fixed in:** v5.15-rc1
**Buggy since:** v5.11-rc1 (introduced by `ae7927023243` "sched: Optimize finish_lock_switch()")

## Bug Description

The `balance_push()` mechanism is part of the CPU hotplug path in the Linux scheduler. When a CPU is being taken offline, `balance_push_set()` installs `balance_push_callback` as the run-queue's `balance_callback`. This ensures that every time the scheduler runs `__balance_callbacks()` on that dying CPU, any non-per-CPU task currently running is migrated away via a stop-machine work item (`__balance_push_cpu_stop`). This is critical for safely draining a CPU of all user tasks before it goes fully offline.

The bug arises because `balance_push()` can be invoked not only from the local (dying) CPU, but also from a **remote** CPU. Specifically, when `sched_setscheduler()` or `rt_mutex_setprio()` changes the priority or scheduling class of a task that happens to reside on the dying CPU's run-queue, the balance callback chain for that remote rq is executed on the calling CPU. Since `balance_push()` is installed as the callback on the dying CPU's rq, it gets invoked on the remote CPU that called `sched_setscheduler()` or `rt_mutex_setprio()`.

Before the fix introduced by commit `ae7927023243`, the scheduler had a separate `balance_switch()` function that was called in `finish_lock_switch()` and handled the `BALANCE_PUSH` flag differently from regular balance callbacks. The optimization in `ae7927023243` unified these paths by replacing `balance_switch()` with a direct call to `__balance_callbacks()`, which processes all queued callbacks including `balance_push_callback`. This meant that `balance_push()` could now be invoked from any code path that calls `__balance_callbacks()` or `balance_callbacks()` on the dying CPU's rq — including the `sched_setscheduler()` and `rt_mutex_setprio()` paths that operate on remote run-queues.

The original code had a `SCHED_WARN_ON(rq->cpu != smp_processor_id())` assertion that would fire when this remote invocation occurred, but lacked any guard to prevent the function from continuing to execute its hotplug logic on the wrong CPU.

## Root Cause

The root cause lies in the interaction between two scheduler mechanisms: (1) the balance callback infrastructure used during CPU hotplug, and (2) the `__balance_callbacks()` / `balance_callbacks()` invocation sites in priority/policy change paths.

When `balance_push_set(cpu, true)` is called during CPU hotplug teardown, it sets `rq->balance_callback = &balance_push_callback` on the dying CPU's rq. The `balance_push_callback` is a global `struct callback_head` whose `.func` points to `balance_push()`.

The `__balance_callbacks()` function calls `splice_balance_callbacks(rq)` which removes the callback from the rq and then invokes `do_balance_callbacks()` to execute each callback function. The key observation is that `__balance_callbacks()` does not check whether the calling CPU is the same CPU that owns the rq — it simply invokes whatever callback was installed.

There are four sites in `core.c` where balance callbacks are executed:

1. **`finish_lock_switch()` (line ~4448):** Called after context switch; always runs on the local CPU. Safe.
2. **`__schedule()` prev==next path (line ~6031):** Called when the scheduler picks the same task; always runs on the local CPU. Safe.
3. **`rt_mutex_setprio()` (line ~6582):** Calls `__balance_callbacks(rq)` directly. The `rq` here belongs to the task whose priority is being changed, which can be on any CPU. The calling CPU may differ from `rq->cpu`. **Unsafe.**
4. **`__sched_setscheduler()` (line ~7187-7196):** Uses `splice_balance_callbacks(rq)` followed by `balance_callbacks(rq, head)` after dropping the rq lock. Again, `rq` can be remote. **Unsafe.**

When `balance_push()` executes on a remote CPU for a dying CPU's rq, several things go wrong:

- The `SCHED_WARN_ON(rq->cpu != smp_processor_id())` fires, producing a kernel warning splat.
- The function reads `rq->curr` which is the currently running task on the dying CPU, not the calling CPU. While this is the intended task for the dying CPU case, operating on it from a remote CPU is incorrect.
- Most critically, the function calls `stop_one_cpu_nowait(rq->cpu, __balance_push_cpu_stop, push_task, this_cpu_ptr(&push_work))`. Here, `this_cpu_ptr(&push_work)` resolves to the **calling CPU's** per-CPU `push_work`, not the dying CPU's. This `push_work` variable is not serialized by the rq lock or any busy flag (unlike `rq->push_work` used by RT/DL push callbacks which have `rq->push_busy` serialization). If two remote CPUs simultaneously invoke `sched_setscheduler()` for tasks on the dying rq, they could both try to use `this_cpu_ptr(&push_work)`, or the dying CPU could also be using its own `push_work` concurrently, leading to double enqueues on the stop-machine work list.

The fix adds `rq != this_rq()` to the early-return condition: `if (!cpu_dying(rq->cpu) || rq != this_rq()) return;`. This ensures that `balance_push()` only proceeds with its hotplug migration logic when it is actually running on the CPU that owns the rq (i.e., the dying CPU itself). Remote invocations harmlessly return after re-installing the callback.

## Consequence

The primary observable consequence is a **kernel warning** (SCHED_WARN_ON splat) triggered when `balance_push()` is invoked on a remote CPU. This produces a stack trace in `dmesg` / kernel logs, which is disruptive but not immediately fatal.

The more serious consequence is **data corruption of the stop-machine work list**. The per-CPU `push_work` variable is a `struct cpu_stop_work` that gets enqueued onto the stop-machine pending list via `stop_one_cpu_nowait()`. When `balance_push()` runs on the wrong CPU, it uses `this_cpu_ptr(&push_work)` — the calling CPU's `push_work` rather than the dying CPU's. If the calling CPU's `push_work` is already enqueued (e.g., from a previous invocation or a concurrent balance callback), this results in a **double enqueue** on the stop-machine list, corrupting the linked list data structure. This can lead to infinite loops when the stop-machine worker processes its pending list, effectively causing a **CPU hang or soft lockup**. In the worst case, it can cause use-after-free if a `push_work` entry is freed while still linked in the list.

Additionally, because the function operates on `rq->curr` of the dying CPU from a remote CPU without proper synchronization of the `push_work` per-CPU variable, there is a race window where the actual dying CPU might also be executing `balance_push()` for the same rq simultaneously (as part of its normal `__schedule()` → `finish_lock_switch()` → `__balance_callbacks()` path). Both the local and remote executions would call `stop_one_cpu_nowait()` with potentially conflicting `push_work` instances, compounding the corruption.

## Fix Summary

The fix makes two changes to the `balance_push()` function:

First, the `SCHED_WARN_ON(rq->cpu != smp_processor_id())` assertion is removed. This warning was correct in diagnosing that `balance_push()` should not run on a remote CPU, but it was incomplete — it only warned without preventing the problematic behavior. Removing it eliminates the noisy splat while the actual guard (added next) prevents the bug.

Second, the early-return condition is changed from `if (!cpu_dying(rq->cpu)) return;` to `if (!cpu_dying(rq->cpu) || rq != this_rq()) return;`. The added `rq != this_rq()` check ensures that if `balance_push()` is invoked via a balance callback from a remote CPU (e.g., through `sched_setscheduler()` or `rt_mutex_setprio()`), the function returns immediately without executing any of the hotplug migration logic. The callback is still re-installed (`rq->balance_callback = &balance_push_callback`) before the check, ensuring persistence of the callback for the next invocation on the actual dying CPU.

This fix is correct and complete because: (a) the `balance_push()` hotplug migration logic (reading `rq->curr`, calling `stop_one_cpu_nowait()` with `this_cpu_ptr(&push_work)`) is only meaningful when executed on the dying CPU itself; (b) remote invocations were never intended to perform migration work — they merely happen as a side effect of the unified balance callback mechanism; and (c) the callback re-installation ensures the dying CPU will still process the callback during its own scheduling cycles.

## Triggering Conditions

The bug requires the following precise conditions:

1. **CPU hotplug in progress:** A CPU must be in the process of going offline, specifically at the stage where `balance_push_set(cpu, true)` has been called (which happens at `CPUHP_AP_SCHED_STARTING` teardown), so that `cpu_dying(rq->cpu)` returns true and `balance_push_callback` is installed on the dying CPU's rq.

2. **Task on the dying CPU's rq:** There must be at least one task enqueued on the dying CPU's run-queue that is not a per-CPU kthread and does not have migration disabled.

3. **Remote priority/policy change:** Another CPU must call `sched_setscheduler()` (e.g., via the `sched_setscheduler` syscall) or `rt_mutex_setprio()` (e.g., due to priority inheritance from an RT mutex) for a task that is currently on the dying CPU's rq. This causes the balance callback chain to be invoked on the remote CPU for the dying CPU's rq.

4. **Timing window:** The remote priority change must occur during the window between `balance_push_set(cpu, true)` and the completion of the CPU offline process. This window exists throughout the CPU teardown sequence.

The bug is triggered deterministically whenever conditions 1-3 are met simultaneously. There is no race condition in the triggering itself — any remote `sched_setscheduler()` or `rt_mutex_setprio()` call targeting a task on a dying CPU's rq will invoke `balance_push()` on the wrong CPU. The data corruption consequence (double enqueue) requires the additional condition that the per-CPU `push_work` is already in use, which depends on concurrent stop-machine activity.

No special kernel configuration is required beyond `CONFIG_SMP=y` and `CONFIG_HOTPLUG_CPU=y` (both standard on multiprocessor systems). The bug affects all architectures. A minimum of 2 CPUs is required (one dying, one performing the remote priority change).

## Reproduce Strategy (kSTEP)

**This bug CANNOT be reproduced with kSTEP.**

1. **WHY can this bug not be reproduced with kSTEP?**
   The bug fundamentally requires **CPU hotplug** — specifically, a CPU transitioning through the `cpu_dying()` state as part of the hotplug teardown sequence. The `balance_push()` function's migration logic is guarded by `cpu_dying(rq->cpu)`, which checks the per-CPU `cpuhp_state` machine state. Without a CPU actually going through the hotplug offline sequence, `cpu_dying()` always returns false, and `balance_push()` returns immediately without reaching the buggy code path.

   kSTEP does not provide any mechanism to simulate CPU hotplug events. The `kstep_topo_*` APIs configure the static topology (SMT, cluster, package, NUMA node relationships) but do not support dynamic CPU online/offline transitions. There is no `kstep_cpu_offline()` or `kstep_cpu_hotplug()` function.

   Additionally, the bug requires the `sched_setscheduler()` syscall or `rt_mutex_setprio()` to be invoked from a remote CPU targeting a task on the dying CPU. While kSTEP's `kstep_task_set_prio()` can change task priorities, the underlying scheduler path and whether it invokes balance callbacks on the correct rq depends on the full kernel hotplug state being consistent.

2. **WHAT would need to be added to kSTEP to support this?**
   Reproducing this bug would require a fundamental addition to kSTEP:
   
   - **`kstep_cpu_offline(int cpu)`**: A function that initiates the CPU hotplug teardown sequence for the specified CPU, transitioning it through the `CPUHP_AP_SCHED_STARTING` teardown state which calls `balance_push_set(cpu, true)`. This is a complex kernel operation involving the cpuhp state machine, stop-machine, migration of tasks and interrupts, and coordination between the dying CPU and the controlling CPU.
   
   - **`kstep_cpu_online(int cpu)`**: A complementary function to bring the CPU back online after testing, since kSTEP needs to maintain a functional system.
   
   These are not minor extensions — CPU hotplug is one of the most complex subsystems in the kernel, involving dozens of callbacks across multiple subsystems (scheduler, RCU, timers, interrupts, workqueues, etc.). Simulating it partially (e.g., just setting `cpu_dying()` to return true without the full state machine) would likely crash the kernel or produce meaningless results, as the hotplug infrastructure depends on consistent state across all subsystems.

3. **Alternative reproduction methods outside kSTEP:**
   The bug can be reproduced on real or virtualized hardware (including QEMU, but not via kSTEP's kernel module) by:
   
   - Booting a kernel between v5.11-rc1 and v5.15-rc1 with `CONFIG_HOTPLUG_CPU=y` on an SMP system (≥2 CPUs).
   - Creating a CFS or RT task pinned to a specific CPU (e.g., CPU 1).
   - On another CPU (e.g., CPU 0), starting a loop that repeatedly calls `sched_setscheduler()` to change the task's scheduling policy between SCHED_OTHER and SCHED_FIFO.
   - Concurrently, using `/sys/devices/system/cpu/cpu1/online` to offline CPU 1 (writing `0`).
   - When the hotplug teardown reaches the `balance_push_set()` stage while `sched_setscheduler()` is executing for a task on CPU 1 from CPU 0, the bug triggers.
   - With `CONFIG_SCHED_DEBUG=y`, the `SCHED_WARN_ON` splat would be visible in `dmesg`. Without it, the double-enqueue corruption on the stop-machine list could manifest as a soft lockup or hang.

   A simple shell script approach:
   ```bash
   # Pin a task to CPU 1
   taskset -c 1 sleep 1000 &
   PID=$!
   # On CPU 0, toggle scheduling policy in a loop
   while true; do chrt -f -p 10 $PID; chrt -o -p 0 $PID; done &
   # Concurrently offline CPU 1
   echo 0 > /sys/devices/system/cpu/cpu1/online
   ```

4. **Additional context from the mailing list thread:**
   The patch was originally reported by Sebastian Siewior (bigeasy@linutronix.de), likely observed in the PREEMPT_RT development tree where CPU hotplug paths are exercised more aggressively. Thomas Gleixner's V1 patch had a logic error (`&&` instead of `||` in the guard condition) which he corrected in V2. Tao Zhou's reply confirmed the analysis: `sched_setscheduler()` and `rt_mutex_setprio()` are the two entry points where `__balance_callbacks()` can be invoked for a remote rq, and the `balance_push()` callback is fundamentally different from RT/DL push callbacks because it lacks the `rq->push_busy` serialization that protects the regular push work.
