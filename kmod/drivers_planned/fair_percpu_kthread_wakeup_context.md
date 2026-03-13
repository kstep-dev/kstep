# Fair: Spurious Per-CPU Kthread Stacking in select_idle_sibling from Interrupt Context

**Commit:** `8b4e74ccb582797f6f0b0a50372ebd9fd2372a27`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.17-rc1
**Buggy since:** v5.6-rc2 (commit 52262ee567ad "sched/fair: Allow a per-CPU kthread waking a task to stack on the same CPU, to fix XFS performance regression")

## Bug Description

The `select_idle_sibling()` function in CFS contains a special-case optimization for tasks woken up by per-CPU kthreads. When a per-CPU kthread (such as a bound kworker) wakes a task, the scheduler allows the wakee to "stack" on the same CPU as the kthread rather than migrating it to an idle CPU. The rationale is that in IO completion patterns (e.g., XFS blk_mq_requeue_work), the kthread will soon go back to sleep, effectively making this a sync wakeup — the wakee should stay on the same CPU to avoid unnecessary migration and cache thrashing.

However, the detection mechanism for this per-CPU kthread wakeup is flawed. The code checks `is_per_cpu_kthread(current)` to determine if the waker is a per-CPU kthread, but `current` is simply the task executing on the CPU at the time `select_idle_sibling()` runs, which is not necessarily the actual waker. If a task wakeup is triggered from interrupt context (e.g., an hrtimer expiration, an IPI, or a softirq), `current` on that CPU could be a per-CPU kthread that has nothing to do with the wakeup. The scheduler would then incorrectly treat this as a per-CPU kthread waking the task and force the wakee to stack on a potentially busy CPU, bypassing the normal idle CPU search entirely.

A related change in the kernel made the idle task (swapper/0, swapper/1, etc.) behave like a per-CPU kthread for the purposes of `is_per_cpu_kthread()`, since the idle task has `PF_KTHREAD` set and `nr_cpus_allowed == 1`. This dramatically increased the frequency of the bug because any wakeup from interrupt context on an otherwise-idle CPU (where swapper is `current`) would now spuriously trigger the kthread stacking path. This turned a relatively uncommon scenario into a very frequent one, causing widespread suboptimal task placement.

## Root Cause

The root cause lies in the incomplete context detection in `select_idle_sibling()` at approximately line 6398 of `kernel/sched/fair.c`. The buggy code is:

```c
if (is_per_cpu_kthread(current) &&
    prev == smp_processor_id() &&
    this_rq()->nr_running <= 1) {
    return prev;
}
```

This code path was introduced by commit 52262ee567ad to handle the XFS IO completion pattern where a bound kworker wakes a task. The three conditions checked are: (1) `current` is a per-CPU kthread (`PF_KTHREAD` flag set and `nr_cpus_allowed == 1`), (2) the wakee's previous CPU (`prev`) is the same as the current CPU (`smp_processor_id()`), and (3) the run queue has at most one running task (`nr_running <= 1`).

The fundamental flaw is condition (1). The `current` pointer reflects whatever task was executing on the CPU when the interrupt fired, not the entity that initiated the wakeup. The call chain for a timer-driven wakeup is: `hrtimer_interrupt()` → `__hrtimer_run_queues()` → timer callback → `wake_up_process()` → `try_to_wake_up()` → `select_task_rq_fair()` → `select_idle_sibling()`. Throughout this entire chain, `current` on the CPU remains the task that was preempted by the interrupt — which could be any per-CPU kthread or even the idle task.

The `is_per_cpu_kthread()` function in `kernel/sched/sched.h` checks two properties:

```c
static inline bool is_per_cpu_kthread(struct task_struct *p)
{
    if (!(p->flags & PF_KTHREAD))
        return false;
    if (p->nr_cpus_allowed != 1)
        return false;
    return true;
}
```

The idle task (swapper) has `PF_KTHREAD` set and is pinned to a single CPU, so `is_per_cpu_kthread(swapper)` returns true. This means that on an idle CPU (where swapper is `current`), any wakeup from timer or IPI context would match condition (1). Combined with the other conditions being trivially satisfied on an idle CPU with `prev == this_cpu` (condition 2) and `nr_running == 0 <= 1` (condition 3, since only swapper is "running" but not counted in nr_running), the stacking path fires. However, in the idle CPU case, step 1 of `select_idle_sibling()` (`available_idle_cpu(target)`) would typically already return `target` before reaching the buggy path, so the swapper scenario mainly matters when `target != prev` or when the idle check fails for other reasons.

The more impactful scenario is when a legitimate per-CPU kthread (like a bound kworker) is running and an unrelated interrupt fires on the same CPU, triggering a wakeup. In this case, the CPU is not idle (the kthread is running), so step 1 of `select_idle_sibling()` does not return early, and the buggy step 3 directly returns `prev` without searching for a truly idle CPU.

## Consequence

The primary consequence is suboptimal task placement leading to performance degradation. When the buggy path fires, the scheduler forces the wakee to share a CPU with a running per-CPU kthread instead of placing it on an available idle CPU. This leads to:

1. **Unnecessary CPU contention**: The wakee competes with the per-CPU kthread for CPU time on the same core, even though other cores may be completely idle. This increases scheduling latency for both the wakee and the kthread.
2. **Increased tail latencies**: Applications with latency-sensitive wakeup paths (e.g., timer-based periodic tasks, network packet processing) may experience unexpected delays because they are placed on busy CPUs instead of idle ones.
3. **Reduced throughput**: By leaving idle CPUs unused and piling tasks on busy ones, overall system throughput decreases, particularly on systems with many cores where there are typically idle CPUs available.
4. **Power inefficiency**: On systems with power management (especially ARM big.LITTLE or similar asymmetric architectures, where this bug was discovered), incorrect task placement can lead to suboptimal frequency scaling and power consumption. The idle CPUs that should be used are left idle while the busy CPU is further loaded.

This bug does not cause crashes, hangs, or data corruption. It is purely a performance and efficiency issue. The impact is most pronounced on systems with multiple CPUs sharing an LLC where idle CPUs are readily available, and on workloads that heavily rely on timer-based or interrupt-based wakeups. The bug was reported by ARM engineers working on mobile/embedded platforms where task placement quality directly affects power consumption and responsiveness.

## Fix Summary

The fix adds a single additional check `in_task()` to the per-CPU kthread stacking condition:

```c
if (is_per_cpu_kthread(current) &&
    in_task() &&
    prev == smp_processor_id() &&
    this_rq()->nr_running <= 1) {
    return prev;
}
```

The `in_task()` macro (defined in `include/linux/preempt.h`) checks whether the kernel is currently executing in task context as opposed to interrupt context. It is defined as:

```c
#define in_task()  (!(in_nmi() | in_hardirq() | in_serving_softirq()))
```

This returns true only when the code is not in NMI context, not in hardirq context, and not in softirq context. By adding this check, the scheduler ensures that the per-CPU kthread stacking optimization only fires when `current` is actually executing in task context — meaning `current` is genuinely the entity performing the wakeup, not just a task that happened to be preempted by an interrupt.

The fix is correct and complete because: (a) in the intended use case (a per-CPU kthread like a bound kworker directly calling `wake_up_process()`), the code runs in task context and `in_task()` returns true, preserving the optimization; (b) in the spurious case (an hrtimer, IPI, or softirq triggering the wakeup while a per-CPU kthread or idle task is `current`), `in_task()` returns false, correctly bypassing the stacking path and allowing normal idle CPU selection to proceed. The patch evolved from v1 (which used `is_idle_thread()` to only exclude the swapper case) to v2 (which uses `in_task()` to exclude all interrupt contexts), based on review feedback from Vincent Guittot who pointed out that the problem is not limited to swapper.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger the bug:

1. **Multi-CPU system**: At least 2 CPUs (plus CPU 0 for the driver in kSTEP). The bug manifests when there are idle CPUs available that SHOULD be chosen but are not.

2. **Per-CPU kthread running on CPU X**: A per-CPU kthread (a kernel thread with `PF_KTHREAD` set and `nr_cpus_allowed == 1`) must be the `current` task on CPU X. This can be any bound kworker, a pinned kthread, or even the idle task (swapper) on kernels where `is_per_cpu_kthread(swapper)` returns true.

3. **Interrupt-context wakeup on CPU X**: A task must be woken up from interrupt context on CPU X. This can happen via: an hrtimer callback (e.g., `nanosleep` expiration, POSIX timer), a network softirq completing and waking a blocked reader, an IPI delivering a wakeup, or any other interrupt-driven wakeup mechanism. The critical point is that `in_task()` returns false during the wakeup call chain.

4. **Task's previous CPU is CPU X**: The woken task's `prev_cpu` (the CPU it last ran on) must equal CPU X (the CPU where the interrupt-context wakeup occurs). This makes `prev == smp_processor_id()` true.

5. **Low runqueue occupancy on CPU X**: The runqueue on CPU X must have `nr_running <= 1` (at most the per-CPU kthread itself is running, and nothing else is queued). This ensures the third sub-condition of the stacking check passes.

6. **CPU X is not considered idle**: For the bug to have visible impact, CPU X must NOT pass the `available_idle_cpu(target)` check in step 1 of `select_idle_sibling()`. This means a per-CPU kthread (not swapper) must be `current`, making `idle_cpu()` return false. If the idle task is `current`, the CPU would be detected as idle in step 1 and the correct CPU would be returned before reaching the buggy path.

7. **Idle CPU available elsewhere**: For the bug to cause observable harm, there must be at least one other idle CPU (e.g., CPU Y) in the same LLC domain that would be selected by the normal idle scan path. If no idle CPUs are available, the buggy and fixed paths would arrive at similar results.

The bug is deterministic given these conditions — it is not a race condition. Whenever an interrupt-context wakeup occurs on a CPU running a per-CPU kthread with the above conditions met, the stacking path fires incorrectly 100% of the time. The reliability of reproduction depends on how easily one can arrange for an interrupt-context wakeup to occur while a per-CPU kthread is `current` on the same CPU.

## Reproduce Strategy (kSTEP)

The reproduction strategy uses kSTEP to create a controlled scenario where a task is woken from interrupt context on a CPU running a per-CPU kthread, then observes whether the task is incorrectly placed on the same CPU (buggy) or correctly placed on an idle CPU (fixed).

### Setup

1. **CPU configuration**: Use at least 3 CPUs in QEMU: CPU 0 (driver), CPU 1 (per-CPU kthread + interrupt wakeup), CPU 2 (idle, should be chosen by fixed kernel). All CPUs share a single LLC (default topology).

2. **Create a per-CPU kthread on CPU 1**: Use `kstep_kthread_create("pcpu_kt")` to create a kernel thread, then `kstep_kthread_bind(kt, cpumask_of(1))` to pin it to CPU 1, and `kstep_kthread_start(kt)` to start it. This kthread should run continuously (e.g., in a busy loop or performing periodic work) to keep CPU 1 non-idle.

3. **Create a CFS task**: Use `kstep_task_create()` to create a CFS task. Pin it temporarily to CPU 1 with `kstep_task_pin(task, 1, 1)`, let it run for a few ticks so its `prev_cpu` is set to CPU 1, then block it with `kstep_task_block(task)`. After blocking, unpin it (or set wide affinity to CPUs 1-2) so it CAN be placed on CPU 2 by the fixed kernel.

4. **Ensure CPU 2 is idle**: Do not schedule any tasks on CPU 2. It should remain idle (swapper running).

### Triggering the Bug

5. **Wake the task from interrupt context on CPU 1**: Use `smp_call_function_single(1, wake_fn, task, 1)` where `wake_fn` calls `wake_up_process(task)`. The `smp_call_function_single()` sends an IPI to CPU 1, and the callback runs in IPI/interrupt context on CPU 1. At this point, `current` on CPU 1 is the per-CPU kthread, `in_task()` returns false, and `smp_processor_id()` returns 1. Alternatively, set up an hrtimer on CPU 1 using `hrtimer_init()` + `hrtimer_start()` with a callback that calls `wake_up_process()`. Both approaches achieve interrupt-context wakeup on CPU 1.

   The IPI approach (`smp_call_function_single`) is simpler and more deterministic because it executes synchronously — the driver waits for the IPI to complete, ensuring the wakeup has happened before proceeding to observation.

### Observation

6. **Check task placement after wakeup**: After the `smp_call_function_single()` returns, use `task_cpu(task)` to check which CPU the task was placed on. Also call `kstep_sleep()` to let the scheduler settle, then read `task_cpu(task)` again.

7. **Pass/fail criteria**:
   - **Buggy kernel**: `task_cpu(task) == 1`. The buggy code returns `prev` (CPU 1) from the kthread stacking path, even though the wakeup came from IPI context, not from the kthread itself. The task is forced to share CPU 1 with the per-CPU kthread.
   - **Fixed kernel**: `task_cpu(task) == 2` (or any other idle CPU). The `in_task()` check correctly identifies that the wakeup is from interrupt context, skips the stacking path, and the normal idle CPU search finds CPU 2.

### Implementation Notes

8. **Using raw kernel APIs**: The driver will need to use `smp_call_function_single()` (or hrtimers) directly, which is a standard kernel API accessible from any kernel module. This does not require changes to the kSTEP framework — it is simply using kernel facilities from within the driver, similar to how other kSTEP drivers use `KSYM_IMPORT` and direct kernel structure access.

9. **Ensuring the per-CPU kthread remains current**: The kthread must be the running task on CPU 1 when the IPI arrives. After `kstep_kthread_start()`, give a few ticks (`kstep_tick_repeat(5)`) to ensure the kthread is scheduled and running on CPU 1. Verify with `cpu_rq(1)->curr` that the kthread is indeed `current` on CPU 1 before proceeding.

10. **Task affinity setup**: After blocking the CFS task, change its CPU affinity to include both CPU 1 and CPU 2 (e.g., `set_cpus_allowed_ptr(task, cpumask)` covering CPUs 1-2). This allows the scheduler to choose CPU 2 on the fixed kernel. If affinity is pinned to CPU 1 only, both kernels would place the task on CPU 1 regardless.

11. **Logging**: Add `kstep_pass()`/`kstep_fail()` calls based on `task_cpu(task)` after wakeup. On buggy kernel, expect CPU 1 (and call `kstep_fail("Task placed on CPU %d, expected != 1", task_cpu(task))`). On fixed kernel, expect CPU 2 (and call `kstep_pass(...)`). Also log intermediate state: `cpu_rq(1)->curr`, `cpu_rq(1)->nr_running`, `in_task()` value inside the IPI callback (should be 0), and the resulting `task_cpu(task)`.

12. **Kernel version guard**: Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,0) && LINUX_VERSION_CODE < KERNEL_VERSION(5,17,0)` since the bug exists from v5.6 through v5.16 and kSTEP supports v5.15+.
