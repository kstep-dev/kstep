# Core: stop_one_cpu_nowait() vs CPU Hotplug Preemption Race

**Commit:** `f0498d2a54e7966ce23cd7c7ff42c64fa0059b07`
**Affected files:** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/rt.c, kernel/sched/deadline.c
**Fixed in:** v6.7-rc1
**Buggy since:** v5.11-rc1 (introduced by `6d337eab041d` "sched: Fix migrate_disable() vs set_cpus_allowed_ptr()")

## Bug Description

A race condition exists between `stop_one_cpu_nowait()` and CPU hotplug that can cause `affine_move_task()` to become permanently stuck in `wait_for_completion()`, triggering a hung-task detector warning. The bug was reported by Kuyo Chang at MediaTek during a `sched_setaffinity()` vs CPU hotplug stress test on arm64 hardware with `PREEMPT_FULL=y`.

The race arises in the scheduler's task migration machinery. When `sched_setaffinity()` is called to change a task's CPU affinity, the code path goes through `__set_cpus_allowed_ptr()` → `__set_cpus_allowed_ptr_locked()` → `affine_move_task()`. If the target task is currently running on a CPU, the migration cannot happen immediately — instead, a `migration_cpu_stop` stopper work item is queued via `stop_one_cpu_nowait()` to perform the migration asynchronously. The caller then blocks in `wait_for_completion()` until the stopper work completes the migration and calls `complete_all()`.

The critical problem is that `stop_one_cpu_nowait()` was called after dropping the runqueue lock (`task_rq_unlock()`), which creates a window where preemption can occur. If, during that preemption window, the target CPU undergoes a hotplug-down operation, the CPU's stopper thread gets disabled (`stopper->enabled = false`). When control returns and `stop_one_cpu_nowait()` is finally called, it finds the stopper disabled and silently returns `false` — the migration stopper work is never queued, the `complete()` callback never fires, and the caller hangs indefinitely.

The same pattern existed in seven distinct call sites across four scheduler files: `affine_move_task()` (three sites in core.c), `migration_cpu_stop()` (one site in core.c), `balance_push()` (one site in core.c), `push_rt_task()` / `pull_rt_task()` (two sites in rt.c), `pull_dl_task()` (one site in deadline.c), and `load_balance()` (one site in fair.c).

## Root Cause

The root cause is a missing atomicity guarantee between the rq-lock check for CPU online status and the subsequent `stop_one_cpu_nowait()` call. The scheduler code verified the CPU was online while holding the rq-lock (which serializes with hotplug), but then dropped the lock before calling `stop_one_cpu_nowait()`. Since `stop_one_cpu_nowait()` must issue a wakeup, it cannot be called under the scheduler locks (lock ordering constraint). This creates an inherent race window.

The exact race scenario in `affine_move_task()` proceeds as follows:

1. **CPU0** calls `__set_cpus_allowed_ptr()` on a task running on CPU1. It acquires the task's `pi_lock` and CPU1's `rq->lock` via `task_rq_lock()`.

2. Meanwhile, **CPU1** is undergoing `_cpu_down()` → `takedown_cpu()` → `stop_machine_cpuslocked(take_cpu_down, ..)`. The multi-stop state machine has the stopper threads on all CPUs executing in lockstep.

3. CPU0's stopper thread gets preempted into (`PREEMPT: cpu_stopper_thread()`), which processes the `MULTI_STOP_PREPARE` state for the multi-stop operation.

4. Back on CPU0, `affine_move_task()` determines the task is on-CPU (`task_on_cpu(rq, p)` is true), sets up the pending migration, and then calls `task_rq_unlock()` to drop `rq->lock` and `pi_lock`.

5. CPU0's stopper thread is preempted in again. It processes `ack_state()`. Meanwhile on CPU1, the multi-stop reaches `MULTI_STOP_RUN`, which calls `take_cpu_down()` → `__cpu_disable()` → `stop_machine_park()`. The last call sets `stopper->enabled = false` on CPU1.

6. Control returns to `affine_move_task()` on CPU0, which now calls `stop_one_cpu_nowait(cpu_of(rq), migration_cpu_stop, ...)`. Inside `cpu_stop_queue_work()`, the check `if (stopper->enabled)` finds it is `false` because CPU1's stopper was just disabled. The function returns `false`, and the stopper work is never queued.

7. `affine_move_task()` proceeds to `wait_for_completion(&pending->done)`. Since `migration_cpu_stop` will never run (it was never queued), `complete_all(&pending->done)` is never called, and the task hangs forever in `TASK_UNINTERRUPTIBLE` state.

The key insight is that between `task_rq_unlock()` and `stop_one_cpu_nowait()`, the current task can be preempted (on `PREEMPT_FULL` kernels), which allows the multi-stop CPU hotplug machinery to complete its disable sequence on the target CPU. The lock itself serialized with hotplug, but the serialization was lost the moment the lock was dropped.

## Consequence

The observable impact is a complete hang of the thread that called `sched_setaffinity()`. The thread enters `TASK_UNINTERRUPTIBLE` state in `wait_for_completion()` and never wakes up. After the kernel's hung-task timeout (default 120 seconds, configurable), the khungtaskd watchdog reports a warning.

The actual stack trace from the bug report shows a `stressapptest` process blocked for over 3600 seconds (1 hour):

```
INFO: task stressapptest:17803 blocked for more than 3600 seconds.
Call trace:
 __switch_to+0x17c/0x338
 __schedule+0x54c/0x8ec
 schedule+0x74/0xd4
 schedule_timeout+0x34/0x108
 do_wait_for_completion+0xe0/0x154
 wait_for_completion+0x44/0x58
 __set_cpus_allowed_ptr_locked+0x344/0x730
 __sched_setaffinity+0x118/0x160
 sched_setaffinity+0x10c/0x248
 __arm64_sys_sched_setaffinity+0x15c/0x1c0
```

This is a livelock/hang bug, not a crash — the affected thread is permanently stuck in D-state and cannot be killed. The same race pattern in `balance_push()` could additionally stall CPU hotplug completion itself, since the balance-push mechanism is critical for draining tasks from a CPU being taken offline. In the RT and deadline pull paths, the consequence would be a missed push operation for migration-disabled tasks, potentially leaving RT/DL tasks on suboptimal CPUs, though these cases are less severe since they don't involve `wait_for_completion()`.

## Fix Summary

The fix applies a simple pattern to all seven affected call sites: wrap the sequence of `task_rq_unlock()` (or `raw_spin_rq_unlock()`) followed by `stop_one_cpu_nowait()` in a `preempt_disable()` / `preempt_enable()` pair. The `preempt_disable()` is placed *before* the unlock, and `preempt_enable()` is placed *after* the `stop_one_cpu_nowait()` call.

```c
preempt_disable();
task_rq_unlock(rq, p, rf);
if (!stop_pending)
    stop_one_cpu_nowait(...)
preempt_enable();
```

This works because if preemption is disabled, the current CPU's stopper thread cannot run between the unlock and the `stop_one_cpu_nowait()` call. Since the multi-stop CPU hotplug mechanism requires all CPUs' stopper threads to participate (via `stop_machine_cpuslocked()`), the hotplug-down sequence cannot progress past the multi-stop barriers while any CPU has preemption disabled and is in this critical section. Therefore, if the CPU was verified as online while holding the rq-lock, it will still be online (and its stopper still enabled) when `stop_one_cpu_nowait()` runs.

The fix respects the existing lock ordering constraint — `stop_one_cpu_nowait()` is still called without holding the rq-lock (avoiding the wakeup-under-scheduler-lock issue), while preemption-disable provides the necessary atomicity against concurrent CPU hotplug. The fix was applied to all similar patterns across `core.c` (5 sites), `rt.c` (2 sites), `deadline.c` (1 site), and `fair.c` (1 site).

## Triggering Conditions

- **Kernel configuration:** `CONFIG_PREEMPT` or `CONFIG_PREEMPT_DYNAMIC` with preempt=full mode (default on arm64). A non-preemptible kernel has a much smaller race window (only voluntary preemption points), making the bug far less likely though still theoretically possible at specific code points.

- **CPU count:** At least 2 CPUs required. The race involves one CPU performing `sched_setaffinity()` and a different target CPU undergoing hotplug-down.

- **Workload:** Concurrent stress testing of `sched_setaffinity()` syscalls and CPU hotplug operations (e.g., echoing 0/1 to `/sys/devices/system/cpu/cpuN/online`). The original reporter used `stressapptest` alongside CPU hotplug cycling.

- **Timing requirements:** Extremely tight race window. The preemption must occur in the exact gap between `task_rq_unlock()` and `stop_one_cpu_nowait()`, and during that preemption, the target CPU's stopper thread must be disabled by the hotplug-down sequence. The reporter noted reproduction rate was approximately once per week under continuous stress testing.

- **Architecture:** More easily triggered on arm64 where `PREEMPT_FULL` is the default. x86_64 with `PREEMPT_NONE` (common in server configurations) would have a much lower probability. The race also requires the stopper thread's multi-stop to interleave precisely during the preemption window.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   This bug fundamentally requires CPU hotplug — specifically, the `_cpu_down()` → `takedown_cpu()` → `stop_machine_cpuslocked(take_cpu_down, ..)` → `stop_machine_park()` code path that sets `stopper->enabled = false` on the target CPU. kSTEP runs inside QEMU with a fixed set of online CPUs. There is no mechanism in kSTEP to initiate or simulate CPU hotplug events. The stopper threads in QEMU's guest kernel are always enabled for all online CPUs, so the `stop_one_cpu_nowait()` call will always succeed — the buggy code path where it returns `false` cannot be triggered.

   Additionally, the race requires precise preemption timing: the current thread must be preempted between `task_rq_unlock()` and `stop_one_cpu_nowait()`, and during that preemption, the multi-stop CPU hotplug sequence must advance far enough to disable the target CPU's stopper. kSTEP's tick-driven execution model (`kstep_tick()`) does not provide the sub-tick preemption interleaving needed to create this race window. kSTEP tasks are kernel-controlled and cannot issue `sched_setaffinity()` syscalls — the affinity change must originate from userspace or be triggered programmatically through the `set_cpus_allowed_ptr()` kernel API, but even then, the hotplug side is unavailable.

2. **WHAT would need to be added to kSTEP to support this?**
   To reproduce this bug, kSTEP would need:
   - **CPU hotplug simulation:** A `kstep_cpu_hotplug(int cpu, bool online)` API that triggers the full `_cpu_down()` / `_cpu_up()` sequence, including `takedown_cpu()`, `stop_machine_park()`, and all the associated stopper thread state transitions. This is a fundamental architectural addition, not a minor helper.
   - **Preemption injection:** A mechanism to force a preemption point at a specific instruction within the kernel code (between `task_rq_unlock()` and `stop_one_cpu_nowait()`). This would require either dynamic patching (e.g., inserting `preempt_schedule()` calls) or a controlled way to trigger timer interrupts at precise code locations.
   - **Stopper thread control:** The ability to observe and control the multi-stop state machine's progress, so the hotplug sequence can be advanced to exactly the right point during the injected preemption.

   These are fundamental capabilities involving real kernel subsystem interactions (CPU hotplug, stop_machine, preemption) that go far beyond kSTEP's module-based task simulation architecture.

3. **Alternative reproduction methods:**
   The bug can be reproduced on real hardware or in a full QEMU VM with:
   - A guest kernel with `CONFIG_PREEMPT=y` or `preempt=full` boot parameter.
   - At least 4 CPUs (to increase hotplug/migration contention).
   - Two concurrent stress loops:
     a. Rapid CPU hotplug cycling: `while true; do echo 0 > /sys/devices/system/cpu/cpu$N/online; sleep 0.01; echo 1 > /sys/devices/system/cpu/cpu$N/online; sleep 0.01; done`
     b. Rapid affinity changes targeting the same CPUs: `while true; do taskset -p -c $N $PID; done`
   - Even then, the reproduction rate is extremely low (approximately once per week under continuous testing, per the reporter).
