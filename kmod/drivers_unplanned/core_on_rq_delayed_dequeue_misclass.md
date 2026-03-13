# Core: External p->on_rq Users Misclassify Delayed-Dequeue Tasks

**Commit:** `cd9626e9ebc77edec33023fe95dab4b04ffc819d`
**Affected files:** `kernel/sched/core.c` (comments and documentation only; all functional changes are in external consumers: `kernel/events/core.c`, `kernel/freezer.c`, `kernel/rcu/tasks.h`, `kernel/time/tick-sched.c`, `kernel/trace/trace_selftest.c`, `virt/kvm/kvm_main.c`, `include/linux/sched.h`)
**Fixed in:** v6.12-rc4
**Buggy since:** v6.12-rc1 (introduced by commit `152e11f6df29` "sched/fair: Implement delayed dequeue")

## Bug Description

The "delayed dequeue" feature (commit `152e11f6df29`) was introduced to preserve EEVDF lag information for sleeping tasks. Rather than immediately removing a task from the runqueue when it blocks, the scheduler keeps the task on the runqueue (with `p->on_rq == 1`) and marks it with `p->se.sched_delayed = 1`. The task competes on the runqueue until it is picked again, at which point it is actually dequeued. This prevents tasks from gaming the scheduler by micro-sleeping at the end of their time quantum to reset their lag.

However, this change fundamentally altered the semantics of the `p->on_rq` field. Prior to delayed dequeue, `p->on_rq != 0` was a reliable indicator that a task was runnable. After delayed dequeue, a task can have `p->on_rq == 1` while being effectively blocked — it will not execute again until explicitly woken up. Multiple subsystems outside the scheduler relied on `p->on_rq` as a proxy for "this task is runnable and competing for CPU time," and all of them started producing incorrect results.

The most prominent reporter was Sean Christopherson, who observed that KVM's preemption notifiers began mis-classifying preemption vs. blocking. When a vCPU task is scheduled out while in the delayed-dequeue state, KVM incorrectly marks it as "preempted" and "ready" (as if it was involuntarily descheduled), when it should have been classified as "blocked" (voluntarily gave up the CPU). This directly impacts KVM's guest scheduling hints, which guests rely on to make informed spinlock and scheduling decisions.

The bug affects at least five different subsystems: KVM preemption notifiers, perf context-switch event classification, the process freezer, RCU tasks quiescent-state detection, and ftrace wakeup self-tests.

## Root Cause

The root cause is that commit `152e11f6df29` introduced a new task state — on the runqueue but not truly runnable (delayed dequeue) — without updating the numerous external consumers that check `p->on_rq` to determine task runnability.

Before delayed dequeue, the `p->on_rq` field had a clear meaning:
- `p->on_rq == 0`: task is blocked / sleeping (not on any runqueue)
- `p->on_rq == TASK_ON_RQ_QUEUED (1)`: task is runnable and queued
- `p->on_rq == TASK_ON_RQ_MIGRATING (2)`: task is being migrated

After delayed dequeue, a fourth semantic state was implicitly created:
- `p->on_rq == TASK_ON_RQ_QUEUED (1)` AND `p->se.sched_delayed == 1`: task is on the runqueue but should be treated as blocked; it will be dequeued when next picked by the scheduler

External code that tested `p->on_rq` (or `task_on_rq_queued(p)`) to determine runnability was no longer correct. Specifically:

1. **KVM** (`virt/kvm/kvm_main.c`, `kvm_sched_out()`): Tests `current->on_rq` to decide if the vCPU was preempted (still runnable) vs. blocked (voluntarily sleeping). With delayed dequeue, a blocked vCPU task that entered the delayed-dequeue state would have `on_rq == 1`, causing KVM to set `vcpu->preempted = true` and `vcpu->ready = true` — telling the guest that the vCPU was stolen, not sleeping.

2. **perf events** (`kernel/events/core.c`, `perf_event_switch()`): Tests `task->on_rq` when generating a context-switch perf record to set the `PERF_RECORD_MISC_SWITCH_OUT_PREEMPT` flag. A delayed-dequeue task would incorrectly get this flag, making profiling tools believe the task was preempted when it voluntarily slept.

3. **Freezer** (`kernel/freezer.c`, `__set_task_frozen()`): Tests `p->on_rq` and returns 0 (can't freeze) if the task appears runnable. A delayed-dequeue task would appear runnable, preventing the freezer from freezing it. This is particularly problematic because a delayed-dequeue task will eventually be dequeued by the scheduler — it is safe to freeze.

4. **RCU tasks** (`kernel/rcu/tasks.h`, `rcu_tasks_is_holdout()`): Tests `t->on_rq` to determine if a task is a holdout (preventing a grace period from completing). A delayed-dequeue task would be considered a holdout, potentially delaying RCU grace periods unnecessarily. The fix adds a comment acknowledging this but conservatively does not change the behavior — the delayed state is spurious and will resolve naturally.

5. **ftrace self-test** (`kernel/trace/trace_selftest.c`): Uses `while (p->on_rq)` to busy-wait until a deadline thread sleeps. A delayed-dequeue task would keep `on_rq == 1` indefinitely from this code's perspective, causing the test to hang.

## Consequence

The consequences vary by affected subsystem:

**KVM (most impactful):** Guest VMs receive incorrect scheduling hints. When a vCPU is voluntarily sleeping (e.g., the guest is idle), KVM tells the guest the vCPU was preempted. This can cause guests to make suboptimal decisions: they may spin-wait on locks (expecting the vCPU to be rescheduled soon) instead of yielding, or they may report inflated steal time. On overcommitted hosts, this misclassification can lead to significantly degraded guest performance and incorrect guest CPU accounting. This was the behavior that prompted Sean Christopherson to report the bug.

**perf events:** Performance profiling tools (e.g., `perf record -e context-switches`) receive incorrect `PERF_RECORD_MISC_SWITCH_OUT_PREEMPT` flags. This makes it appear that tasks are being preempted more often than they actually are, leading to incorrect analysis. Developers relying on this data may chase phantom preemption issues.

**Freezer:** Tasks in the delayed-dequeue state cannot be frozen, potentially slowing or blocking system suspend/hibernate operations. The fix notes that it is safe to freeze delayed-dequeue tasks — they will not execute until `ttwu()` revives them, so swapping their state is safe.

**ftrace self-test:** The wakeup latency self-test can hang indefinitely, blocking boot on systems where self-tests are enabled. The `while (p->on_rq)` loop would never terminate for a delayed-dequeue task.

## Fix Summary

The fix introduces a new inline helper function `task_is_runnable()` in `include/linux/sched.h`:

```c
static inline bool task_is_runnable(struct task_struct *p)
{
    return p->on_rq && !p->se.sched_delayed;
}
```

This helper correctly accounts for the delayed-dequeue state: a task is considered runnable only if it is on the runqueue AND is not in the delayed-dequeue state.

The fix then audits all external users of `p->on_rq` and replaces the relevant checks:
- `kernel/events/core.c`: `task->on_rq` → `task_is_runnable(task)` in `perf_event_switch()`
- `kernel/freezer.c`: `p->on_rq` → `task_is_runnable(p)` in `__set_task_frozen()`
- `kernel/trace/trace_selftest.c`: `p->on_rq` → `task_is_runnable(p)` in the wakeup test loop
- `virt/kvm/kvm_main.c`: `current->on_rq` → `task_is_runnable(current)` in `kvm_sched_out()`

For RCU tasks (`kernel/rcu/tasks.h`), the fix adds a comment explaining the situation but conservatively does not change the behavior. The rationale is that the delayed-dequeue state is transient (it will resolve either by the scheduler dequeuing the task or by a wakeup), so treating it as a holdout only affects timing, not correctness.

For `kernel/time/tick-sched.c`, the fix adds comments questioning whether `sched_task_on_rq()` should be replaced with `task_on_cpu()` / `task_curr()` but does not change the code, since tick dependencies are picked up on `schedule()`.

The fix also updates documentation comments in `kernel/sched/core.c` to describe the new `p->on_rq` semantics with delayed dequeue and updates the `task_call_func()` documentation to reference `task_is_runnable()` instead of `->on_rq`.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **Kernel version:** The kernel must include commit `152e11f6df29` (delayed dequeue) but not yet include commit `cd9626e9ebc77edec33023fe95dab4b04ffc819d` (the fix). This corresponds to Linux v6.12-rc1 through v6.12-rc3.

2. **EEVDF delayed dequeue must be active:** The `DELAY_DEQUEUE` scheduler feature must be enabled (it is enabled by default when delayed dequeue is present). A CFS task must enter the delayed-dequeue state, which occurs when a task voluntarily sleeps (`DEQUEUE_SLEEP`) and is not eligible (`!entity_eligible(cfs_rq, se)`) at the time of dequeue.

3. **An external consumer must check the task state while in delayed dequeue:** The specific trigger depends on the subsystem:
   - **KVM:** A vCPU task running under KVM must enter delayed dequeue at the point of context switch. This happens when the vCPU blocks and the scheduler delays its dequeue. The `kvm_sched_out()` preemption notifier fires during context switch and checks `current->on_rq`.
   - **perf:** A perf context-switch event must be generated while the task is being switched out in the delayed-dequeue state.
   - **Freezer:** A system suspend/hibernate (or cgroup freezer) must attempt to freeze a task that is currently in the delayed-dequeue state.
   - **ftrace:** The wakeup self-test must be running, and the test's deadline thread must enter delayed dequeue.

4. **Task must be ineligible at sleep time:** The delayed dequeue only triggers when `entity_eligible(cfs_rq, se)` returns false at the time the task sleeps. This typically happens when the task has accumulated positive lag (ran less than its fair share), meaning the task has used a significant portion of its time slice and the CFS runqueue has enough competing tasks to make the eligibility check fail.

5. **No race with wakeup:** The task must remain in the delayed-dequeue state long enough for the external consumer to observe it. If the task is woken up quickly, `ttwu_runnable()` clears the delayed state before external code sees it.

The bug is relatively easy to trigger in practice on affected kernels because any KVM guest workload with voluntary sleeping (idle vCPUs) will frequently hit the KVM path. The perf path is triggered whenever profiling context switches.

## Reproduce Strategy (kSTEP)

### Why This Bug Cannot Be Reproduced with kSTEP

1. **The bug is not in the scheduler itself.** The scheduler's behavior is identical on both buggy and fixed kernels. The delayed-dequeue mechanism works the same way: tasks enter `on_rq == 1, sched_delayed == 1` state on both kernels. The fix only changes how **external consumers** interpret this state. No code in `kernel/sched/` has functional changes — only comments and documentation are updated in `core.c`.

2. **All affected subsystems are outside kSTEP's reach.** The observable symptoms manifest in:
   - **KVM preemption notifiers** (`virt/kvm/kvm_main.c`): kSTEP runs inside QEMU but does not create nested KVM guests or interact with KVM's vCPU scheduling infrastructure. There is no way to observe `vcpu->preempted` or `vcpu->ready` flags from a kernel module.
   - **perf events** (`kernel/events/core.c`): kSTEP does not create perf event file descriptors, read perf ring buffers, or observe `PERF_RECORD_MISC_SWITCH_OUT_PREEMPT` flags. While theoretically one could open perf events from a kernel module using `perf_event_create_kernel_counter()`, this is far beyond kSTEP's current architecture.
   - **Process freezer** (`kernel/freezer.c`): kSTEP cannot trigger system suspend/hibernate or cgroup freezer operations. The freezer requires `try_to_freeze_tasks()` to iterate over all tasks and attempt to freeze them, which is initiated by the PM subsystem or cgroup freezer controller — neither accessible from kSTEP.
   - **RCU tasks** (`kernel/rcu/tasks.h`): The RCU tasks grace period mechanism is an internal kernel subsystem. While kSTEP could potentially call `synchronize_rcu_tasks()`, observing that a specific task is held as an unnecessary holdout requires instrumenting the RCU internals, and the fix for this subsystem is just a comment (no behavioral change).
   - **ftrace self-test** (`kernel/trace/trace_selftest.c`): This is an internal kernel self-test that runs at boot time. kSTEP cannot trigger or observe its execution.

3. **No scheduler-observable difference between buggy and fixed kernels.** Even if kSTEP created a CFS task and drove it into the delayed-dequeue state (which it can do), the scheduler-internal fields (`on_rq`, `sched_delayed`, `vruntime`, etc.) would be identical on both kernels. The `task_is_runnable()` helper added by the fix is a simple inline function that combines two fields already accessible to kSTEP — but no scheduler decision or scheduling outcome changes between the two kernels. There is no pass/fail criterion expressible in terms of scheduler state or task scheduling order.

4. **The fix is essentially a semantic/API change.** It adds a helper function that external code should use instead of raw `p->on_rq` checks. This is an interface correctness fix, not a behavioral fix within the scheduler. kSTEP is designed to test scheduler behavior (task ordering, preemption decisions, load balancing, etc.), not external subsystem integration.

### What Would Need to Change in kSTEP

To reproduce this bug, kSTEP would need at least one of:

- **perf event support:** Add `kstep_perf_open_switch_events()` and `kstep_perf_read_switch_record()` to create perf context-switch events and read back the records, checking whether `PERF_RECORD_MISC_SWITCH_OUT_PREEMPT` is set. This would require integrating with the perf kernel API (`perf_event_create_kernel_counter()`, ring buffer reading).
- **Freezer support:** Add `kstep_freeze_task(p)` that calls `freeze_task()` on a specific task and returns whether it succeeded. This would allow testing whether a delayed-dequeue task can be frozen.
- **KVM guest support:** Fundamentally impossible — kSTEP runs inside QEMU and cannot nest KVM guests.

These are significant architectural additions, not minor extensions.

### Alternative Reproduction Methods

Outside kSTEP, the bug can be reproduced by:
1. Running a KVM guest on an affected kernel (v6.12-rc1 to rc3) and monitoring `vcpu->preempted` / `vcpu->ready` (e.g., via KVM debugfs or tracepoints) while guest vCPUs voluntarily sleep.
2. Using `perf record -e context-switches` and checking the `PERF_RECORD_MISC_SWITCH_OUT_PREEMPT` flag on switch-out records for tasks that should be blocking.
3. Attempting system suspend while CFS tasks are in the delayed-dequeue state and observing that the freezer fails or is delayed.
