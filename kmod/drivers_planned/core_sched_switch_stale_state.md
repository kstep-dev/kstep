# Core: trace_sched_switch Reports Stale prev_state After Signal

**Commit:** `8feb053d53194382fcfb68231296fdc220497ea6`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.16-rc1
**Buggy since:** v5.18-rc1 (introduced by `fa2c3254d7cf` "sched/tracing: Don't re-read p->state when emitting sched_switch event")

## Bug Description

The `trace_sched_switch` tracepoint in the Linux kernel scheduler reports a stale `prev_state` value when a task has a pending signal at the time it enters `__schedule()`. Specifically, if a task sets its state to `TASK_INTERRUPTIBLE` (or another interruptible sleep state) and then calls `schedule()`, but a signal becomes pending before the scheduler deactivates the task, the `signal_pending_state()` check in `try_to_block_task()` correctly sets the task back to `TASK_RUNNING`. However, the local variable `prev_state` in `__schedule()` is never updated to reflect this change, causing the `trace_sched_switch` tracepoint to emit the original sleep state (e.g., `TASK_INTERRUPTIBLE`) instead of `TASK_RUNNING`.

This bug was introduced in commit `fa2c3254d7cf`, which changed the tracing infrastructure to avoid re-reading `p->__state` when emitting the `sched_switch` event. Before that commit, the tracepoint would call `task_state_index(p)` which re-read `p->__state` directly, naturally picking up any state changes made by `signal_pending_state()`. After that commit, the tracepoint uses a `prev_state` variable captured earlier in `__schedule()`, but the `signal_pending_state()` code path that resets the task to `TASK_RUNNING` never updates this captured variable.

The bug persisted through the refactoring in commit `7b3d61f6578a` (v6.13-rc1) which extracted the deactivation logic into a separate `try_to_block_task()` helper function. This refactoring passed `prev_state` by value to `try_to_block_task()`, making it structurally impossible for the helper to update the caller's copy even if someone intended it to.

## Root Cause

The root cause lies in the data flow between `__schedule()` and `try_to_block_task()` (or the inline signal_pending_state check in earlier kernels). In `__schedule()`, the task state is captured once:

```c
prev_state = READ_ONCE(prev->__state);
```

This `prev_state` local variable serves two purposes: (1) forming a control dependency for the deactivation path, and (2) being passed to `trace_sched_switch()` for tracing. The control dependency purpose only requires reading the value once, but the tracing purpose requires `prev_state` to reflect the *final* state of the task after all modifications.

In the buggy code, `try_to_block_task()` receives `prev_state` by value:

```c
static bool try_to_block_task(struct rq *rq, struct task_struct *p,
                              unsigned long task_state)  // by value!
{
    int flags = DEQUEUE_NOCLOCK;
    if (signal_pending_state(task_state, p)) {
        WRITE_ONCE(p->__state, TASK_RUNNING);  // task is updated
        return false;                          // but caller's prev_state is NOT
    }
    ...
}
```

When `signal_pending_state()` returns true (because the task has `TIF_SIGPENDING` set and the state includes `TASK_INTERRUPTIBLE`), the function correctly sets `p->__state` to `TASK_RUNNING` and returns `false` to indicate the task was not blocked. However, since `task_state` is a local copy, the caller's `prev_state` retains the original sleep state.

Later in `__schedule()`, the stale `prev_state` is passed to the tracepoint:

```c
trace_sched_switch(preempt, prev, next, prev_state);  // prev_state is stale!
```

The `__trace_sched_switch_state()` function inside the tracepoint uses this `prev_state` to compute the reported state via `__task_state_index(prev_state, p->exit_state)`, which produces the wrong result (e.g., reporting `S` for sleeping instead of `R` for running).

In earlier kernels (v5.18 through v6.12) before the `try_to_block_task()` refactoring, the same bug existed with the signal_pending_state check inline in `__schedule()`:

```c
prev_state = READ_ONCE(prev->__state);
if (!(sched_mode & SM_MASK_PREEMPT) && prev_state) {
    if (signal_pending_state(prev_state, prev)) {
        WRITE_ONCE(prev->__state, TASK_RUNNING);
        // prev_state is never updated here!
    } else {
        deactivate_task(rq, prev, DEQUEUE_SLEEP | DEQUEUE_NOCLOCK);
    }
}
// ...
trace_sched_switch(..., prev_state, ...);  // stale!
```

## Consequence

The primary consequence is incorrect tracing output. The `sched_switch` tracepoint is one of the most widely used scheduler tracepoints, consumed by tools including `trace-cmd`, `perf sched`, `LTTng`, and various custom BPF programs. When this bug triggers, these tools report that a task was voluntarily sleeping (e.g., state `S` for `TASK_INTERRUPTIBLE` or `D` for `TASK_UNINTERRUPTIBLE`) when it was actually still running (state `R` for `TASK_RUNNING`).

This can cause significant confusion during performance debugging and system analysis. For example, `perf sched latency` may incorrectly attribute time to voluntary sleep states, skewing latency histograms. BPF programs that track task state transitions (common in observability tools like `bpftrace` one-liners) would record spurious sleep→wake transitions. The `runqlat` BPF tool, which measures scheduler run queue latency, could produce incorrect results if it relies on `sched_switch` state to determine when a task voluntarily yields vs. is preempted.

While the actual scheduling behavior is correct (the task IS set back to `TASK_RUNNING` and stays on the runqueue), the incorrect trace data can lead developers to misdiagnose performance problems or miss real issues. In automated monitoring systems that parse trace data, false sleep state reports could trigger incorrect alerts or mask genuine scheduling anomalies.

## Fix Summary

The fix changes `try_to_block_task()` to accept `prev_state` by pointer instead of by value. The function signature changes from `unsigned long task_state` to `unsigned long *task_state_p`. Inside the function, a local copy is made for the conditional checks (`unsigned long task_state = *task_state_p`), but when `signal_pending_state()` triggers and the task is set back to `TASK_RUNNING`, the fix also writes `TASK_RUNNING` back through the pointer:

```c
static bool try_to_block_task(struct rq *rq, struct task_struct *p,
                              unsigned long *task_state_p)  // now a pointer
{
    unsigned long task_state = *task_state_p;
    int flags = DEQUEUE_NOCLOCK;
    if (signal_pending_state(task_state, p)) {
        WRITE_ONCE(p->__state, TASK_RUNNING);
        *task_state_p = TASK_RUNNING;  // <-- update caller's copy
        return false;
    }
    ...
}
```

The call site in `__schedule()` changes accordingly:

```c
try_to_block_task(rq, prev, &prev_state);  // pass by pointer
```

This ensures that when `trace_sched_switch(preempt, prev, next, prev_state)` is called later, `prev_state` reflects the actual final state of the task. The fix is minimal, correct, and complete: it only changes the data flow for the signal_pending_state path, which is the only code path that can modify the task state after `prev_state` is captured. The normal deactivation path (where `try_to_block_task` returns `true`) doesn't need to update `prev_state` because the task genuinely did enter the sleep state that was captured.

## Triggering Conditions

To trigger this bug, the following conditions must all be met simultaneously:

1. **Task state**: A task must set its state to `TASK_INTERRUPTIBLE` (or a state that includes `TASK_INTERRUPTIBLE` or `TASK_WAKEKILL`) before calling `schedule()`. This is the standard pattern for interruptible sleeps, used extensively in the kernel for wait queues, mutexes, and other blocking primitives. `TASK_UNINTERRUPTIBLE` alone is NOT sufficient unless `TASK_WAKEKILL` is also set, because `signal_pending_state()` filters on `(state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL))`.

2. **Pending signal**: The task must have `TIF_SIGPENDING` set in its thread flags at the time `signal_pending_state()` is called inside `try_to_block_task()` (or the inline equivalent in older kernels). This flag is set by `signal_wake_up()`, `set_tsk_thread_flag()`, or related signal delivery functions. For `TASK_INTERRUPTIBLE` state, any pending signal suffices. For `TASK_WAKEKILL` (without `TASK_INTERRUPTIBLE`), a fatal signal (`SIGKILL`) must be pending (`__fatal_signal_pending(p)` must return true).

3. **Context switch**: After `signal_pending_state()` triggers (setting the task back to `TASK_RUNNING` but leaving `prev_state` stale), a different task must be selected by `pick_next_task()` so that `prev != next` and the `trace_sched_switch()` tracepoint actually fires. If the same task is re-selected, there is no context switch and no tracepoint emission, so the bug is invisible. This requires at least one other runnable task on the same CPU.

4. **Tracepoint enabled**: The `sched_switch` tracepoint must be enabled (via ftrace, perf, BPF, etc.) for the stale state to be observable. Without tracing, the bug has no externally visible effect since the scheduler itself behaves correctly.

The probability of triggering depends on workload characteristics. In systems with heavy signal traffic and interruptible sleeps (e.g., signal-heavy I/O workloads, or applications using `SIGALRM`/`SIGCHLD` frequently), the `signal_pending_state()` path is hit more often. In a controlled test environment, the bug can be triggered deterministically by setting `TIF_SIGPENDING` on a kthread before it enters an interruptible sleep, provided another task is available on the same CPU to cause a context switch.

## Reproduce Strategy (kSTEP)

The bug can be reproduced using a kSTEP driver that creates controlled conditions for the `signal_pending_state()` path and observes the `trace_sched_switch` tracepoint output. Here is the detailed strategy:

### Task Setup

1. **Create two kthreads** bound to CPU 1 (avoiding CPU 0 which is reserved for the driver):
   - **kthread A ("observer")**: A CFS kthread that simply runs in a loop, providing the "next" task for the context switch. This can be created with `kstep_kthread_create("observer")` and bound to CPU 1 with `kstep_kthread_bind()`.
   - **kthread B ("target")**: A custom kthread (created directly via `kthread_create()` since we need a custom function) that, when signaled by the driver, sets `__set_current_state(TASK_INTERRUPTIBLE)` and calls `schedule()`.

2. **Topology**: At least 2 CPUs. No special topology requirements beyond that. Use `kstep_topo_init()` with default topology.

### Tracepoint Hooking

3. **Register a sched_switch tracepoint handler** in the driver's setup function using `register_trace_sched_switch(my_sched_switch_handler, NULL)`. The handler's signature must match the tracepoint prototype:
   ```c
   void my_sched_switch_handler(void *data, bool preempt,
                                struct task_struct *prev,
                                struct task_struct *next,
                                unsigned int prev_state);
   ```
   Note: The argument order may vary between kernel versions. For the fix's target version (~v6.14), the order is `(preempt, prev, next, prev_state)`.

4. In the handler, check if `prev` is our target kthread B. If so, **record the `prev_state` value** and the **actual `prev->__state`** into shared variables for later inspection. Use atomic operations or a simple flag to coordinate.

### Bug Triggering Sequence

5. **Start both kthreads** on CPU 1 using `kstep_kthread_start()` for kthread A and `wake_up_process()` for the custom kthread B.

6. **Let both kthreads run for a few ticks** with `kstep_tick_repeat(5)` to establish their scheduling state.

7. **Set TIF_SIGPENDING on kthread B** from the driver (running on CPU 0):
   ```c
   set_tsk_thread_flag(target_kthread, TIF_SIGPENDING);
   ```

8. **Signal kthread B to enter interruptible sleep**: Set a flag that kthread B's function checks. When it sees the flag, it executes:
   ```c
   __set_current_state(TASK_INTERRUPTIBLE);
   schedule();
   ```

9. **Advance ticks** with `kstep_tick_repeat(3)` to allow the schedule to occur. When kthread B calls `schedule()`:
   - `prev_state = READ_ONCE(prev->__state)` captures `TASK_INTERRUPTIBLE`
   - `signal_pending_state(TASK_INTERRUPTIBLE, B)` returns true (because `TIF_SIGPENDING` is set)
   - `try_to_block_task()` sets `B->__state = TASK_RUNNING` but does NOT update `prev_state` (BUG)
   - `pick_next_task()` picks kthread A (since B is still runnable but A may have better vruntime, or B might yield to A)
   - `trace_sched_switch(false, B, A, prev_state)` fires with stale `prev_state = TASK_INTERRUPTIBLE`
   - Our tracepoint handler captures `prev_state = TASK_INTERRUPTIBLE` while `B->__state = TASK_RUNNING`

### Bug Detection

10. **Check the captured values** after the tick sequence completes:
    - **Buggy kernel**: The tracepoint handler's `prev_state` argument will be `TASK_INTERRUPTIBLE` (value 1) even though the task was set back to `TASK_RUNNING` (value 0). Call `kstep_fail("stale prev_state: got %u, task __state is %u", captured_prev_state, captured_actual_state)`.
    - **Fixed kernel**: The tracepoint handler's `prev_state` argument will be `TASK_RUNNING` (value 0), matching the actual task state. Call `kstep_pass("prev_state correctly updated to TASK_RUNNING")`.

11. **Cleanup**: Unregister the tracepoint handler with `unregister_trace_sched_switch()`. Clear `TIF_SIGPENDING` on kthread B with `clear_tsk_thread_flag(target_kthread, TIF_SIGPENDING)`. Stop both kthreads.

### Additional Notes

- The `__trace_sched_switch_state()` function processes the raw `prev_state` value: for non-preempted tasks, it calls `__task_state_index(prev_state, p->exit_state)` which uses `fls()` to convert the bitmask to an index. On the buggy kernel, `TASK_INTERRUPTIBLE` (bit 0 set) produces index 1, which maps to 'S' (sleeping). On the fixed kernel, `TASK_RUNNING` (value 0) produces index 0, which maps to 'R' (running). The driver can check either the raw prev_state or the processed state index.

- To ensure a context switch actually happens (so the tracepoint fires), kthread A must be runnable on CPU 1. If kthread B is the only runnable task and gets re-selected as `next`, the tracepoint won't fire. Having kthread A continuously running ensures there's always an alternative task.

- The driver should use `LINUX_VERSION_CODE` guards since: (a) the tracepoint signature changed when `prev_state` was added in fa2c3254d7cf (v5.18), and (b) `try_to_block_task()` was refactored in v6.13. The driver should target kernel versions v5.18+ where the bug exists.

- An alternative detection approach is to use `kstep_kthread_block()` if it uses `TASK_INTERRUPTIBLE` internally, but since `kstep_kthread_block()` likely uses `TASK_UNINTERRUPTIBLE`, a custom kthread function is preferred. If `kstep_kthread_block` can be extended to accept a state parameter (e.g., `kstep_kthread_block_interruptible()`), that would simplify the driver.

- For determinism, the driver controls all timing via `kstep_tick()` and task creation. The signal flag (`TIF_SIGPENDING`) is set before the kthread enters the sleep, ensuring the `signal_pending_state()` path is always taken. There are no race conditions or non-deterministic elements in this setup.
