# LB: detach_tasks() O(n) Iteration with Pinned Tasks Causes Hard Lockup

**Commit:** `2feab2492deb2f14f9675dd6388e9e2bf669c27a`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.10
**Buggy since:** `b0defa7ae03e` ("sched/fair: Make sure to try to detach at least one movable task"), merged in v6.1-rc1

## Bug Description

The `detach_tasks()` function in the CFS load balancer iterates over tasks on a source runqueue to find candidates to migrate to a destination CPU. To bound the cost of this iteration, the function uses `env->loop_max` (set from `sysctl_sched_nr_migrate`, default 32) to cap the number of tasks examined. Commit `b0defa7ae03e` modified this logic to bypass the `loop_max` limit when the `LBF_ALL_PINNED` flag is still set — meaning no movable task has been found yet. The intent was to ensure that a movable task buried deep in a long list of CPU-pinned tasks would eventually be found.

However, this change introduced a critical performance regression. When all or most tasks on a CPU are pinned (i.e., their `cpus_ptr` does not include the destination CPU), the `LBF_ALL_PINNED` flag is never cleared by `can_migrate_task()`. This means the `loop_max` check `(env->loop > env->loop_max && !(env->flags & LBF_ALL_PINNED))` never evaluates to true, and `detach_tasks()` iterates over every single task on the runqueue. Since this code runs with the runqueue lock held and often in softirq context, iterating over thousands of tasks creates an O(n) critical section that can trigger hard lockups and watchdog NMIs.

The bug was observed in production at Google where a user had affined O(10k) threads to a single CPU. When load balancing attempted to pull tasks from that overloaded CPU, `detach_tasks()` would iterate through all 10,000+ pinned tasks without finding any movable one, holding the runqueue lock for the entire duration.

Additionally, commit `b0defa7ae03e` also changed the `LBF_NEED_BREAK` re-entry logic in `sched_balance_rq()` (formerly `load_balance()`). The original code unconditionally jumped to `more_balance` after a break; the buggy commit added a condition `if (env.loop < busiest->nr_running)` that would prevent re-entry once all tasks had been scanned. While this additional condition was meant to avoid infinite re-entry, it was coupled to the unbounded loop logic and is also reverted by the fix.

## Root Cause

The root cause is the interaction between the `LBF_ALL_PINNED` flag and the `loop_max` iteration limit in `detach_tasks()`.

In the buggy code, the loop termination condition in `detach_tasks()` is:

```c
if (env->loop > env->loop_max &&
    !(env->flags & LBF_ALL_PINNED))
    break;
```

The `LBF_ALL_PINNED` flag is initialized to set (`env.flags |= LBF_ALL_PINNED`) at the start of `sched_balance_rq()`, before calling `detach_tasks()`. Inside the loop, for each task `p`, the function calls `can_migrate_task(p, env)`. The `LBF_ALL_PINNED` flag is only cleared when `can_migrate_task()` finds a task whose `cpus_ptr` includes the destination CPU — specifically at line `env->flags &= ~LBF_ALL_PINNED` in `can_migrate_task()`, which runs after the `cpumask_test_cpu(env->dst_cpu, p->cpus_ptr)` check passes.

When all tasks on the source CPU are affined such that none can run on the destination CPU, `can_migrate_task()` returns 0 for every task due to the `cpumask_test_cpu()` check failing. The code path that clears `LBF_ALL_PINNED` is never reached. Consequently, `!(env->flags & LBF_ALL_PINNED)` always evaluates to false (since `LBF_ALL_PINNED` remains set), making the entire break condition `(env->loop > env->loop_max && !(env->flags & LBF_ALL_PINNED))` always false, and the loop never terminates via `loop_max`.

The only remaining termination conditions are:
1. `list_empty(tasks)` — the task list is exhausted (i.e., every single task has been examined)
2. `env->idle && env->src_rq->nr_running <= 1` — the source has drained (does not apply when tasks are pinned and unmovable)
3. `env->loop > env->loop_break` — the "breather" break, which sets `LBF_NEED_BREAK` and breaks out of `detach_tasks()`. However, the caller in `sched_balance_rq()` will re-enter `detach_tasks()` via the `more_balance` label, continuing from where it left off.

The breather break (`SCHED_NR_MIGRATE_BREAK`, default 32) does temporarily release the runqueue lock between iterations of `more_balance`, but the buggy commit's additional check `if (env.loop < busiest->nr_running)` still allows re-entry as long as `env.loop` hasn't scanned all `nr_running` tasks. So effectively, the combined loop across all `more_balance` iterations still scans all N tasks, with brief lock releases every 32 tasks. While the lock releases prevent an absolute deadlock, the aggregate time holding the lock across the softirq is still O(n), and with 10k+ tasks, the total softirq execution time exceeds the hard lockup threshold.

Furthermore, the entire `detach_tasks()` → `sched_balance_rq()` path runs from `sched_balance_softirq()` which is invoked via `SCHED_SOFTIRQ`. When running in softirq context, excessive execution time triggers the kernel's soft lockup detector and eventually the NMI watchdog, producing a hard lockup panic.

## Consequence

The most severe consequence is **hard lockups** (NMI watchdog timeouts). When a CPU has thousands of pinned tasks and load balancing targets it as the busiest CPU, the `detach_tasks()` function iterates through every task with the runqueue lock held (or held in aggregate across breather breaks). With O(10k) tasks, this takes an extraordinarily long time, causing:

1. **Hard lockup / NMI watchdog panic**: The runqueue lock is held for too long, preventing other CPUs from making progress on scheduler operations that require this lock. The NMI watchdog fires, producing a stack trace like:
   ```
   NMI watchdog: Watchdog detected hard LOCKUP on cpu X
   ...
   detach_tasks+0x...
   sched_balance_rq+0x...
   sched_balance_softirq+0x...
   ```

2. **Soft lockup warnings**: Even if the hard lockup threshold is not reached, the extended softirq processing triggers soft lockup warnings as the CPU is monopolized by the load balancer for hundreds of milliseconds or more.

3. **System-wide scheduling stall**: While the runqueue lock for the source CPU is held, no task on that CPU can be scheduled in or out. Other CPUs attempting to wake tasks on that runqueue or perform any operation requiring the lock will spin, cascading the delay across the system.

The bug is deterministically reproducible whenever a significant number of CPU-pinned tasks exist on a single CPU and load balancing is triggered. It does not require a race condition — only the right workload configuration. The commit message explicitly states that Google observed hard lockups in production with a user who affined O(10k) threads to a single CPU.

## Fix Summary

The fix is a clean revert of commit `b0defa7ae03e`. It makes two changes in `kernel/sched/fair.c`:

**Change 1: Restore the unconditional `loop_max` limit in `detach_tasks()`**

The buggy condition:
```c
if (env->loop > env->loop_max &&
    !(env->flags & LBF_ALL_PINNED))
    break;
```
is replaced with:
```c
if (env->loop > env->loop_max)
    break;
```

This restores the hard cap on the number of tasks examined, ensuring `detach_tasks()` always terminates after at most `loop_max` iterations (default 32, configurable via `sysctl_sched_nr_migrate`). Even if all examined tasks are pinned, the function will stop searching. The trade-off is that a movable task buried beyond position `loop_max` in the task list may not be found, but this was the original pre-v6.1 behavior and was deemed acceptable since no one was reporting issues with the original limit.

**Change 2: Restore unconditional `more_balance` re-entry**

The buggy condition in `sched_balance_rq()`:
```c
if (env.flags & LBF_NEED_BREAK) {
    env.flags &= ~LBF_NEED_BREAK;
    if (env.loop < busiest->nr_running)
        goto more_balance;
}
```
is replaced with:
```c
if (env.flags & LBF_NEED_BREAK) {
    env.flags &= ~LBF_NEED_BREAK;
    goto more_balance;
}
```

With the `loop_max` limit restored, this condition is no longer needed — the `loop_max` check in `detach_tasks()` itself ensures termination. The unconditional `goto more_balance` is the original behavior, allowing the load balancer to continue scanning after a breather break without requiring that all tasks have been visited. In practice, after `loop_max` total iterations across breather breaks, `detach_tasks()` will hit the `loop_max` limit and terminate normally.

The fix is correct because it restores the O(loop_max) bound on `detach_tasks()` iteration, eliminating the O(n) worst case. Vincent Guittot (author of the original buggy commit) reviewed and approved the revert, acknowledging that the original problem (failing to find a movable task among many pinned ones) was likely not hitting anyone in practice.

## Triggering Conditions

The bug requires the following specific conditions:

- **Kernel version**: v6.1-rc1 through v6.10-rc5 (kernels containing commit `b0defa7ae03e` but not `2feab2492deb`).
- **Large number of pinned tasks on one CPU**: A single CPU must have many CFS tasks (hundreds to thousands) that are affined to that CPU using `sched_setaffinity()` or `cpuset` cgroups. The more tasks, the longer the lockup. Google observed hard lockups with ~10,000 pinned threads.
- **Tasks must be pinned away from the destination CPU**: The tasks' `cpus_ptr` must not include the destination CPU that load balancing wants to pull tasks to. If even one task is movable, `LBF_ALL_PINNED` gets cleared after examining it, and the `loop_max` limit re-engages.
- **Load imbalance detected**: The load balancer must identify the CPU with the pinned tasks as the "busiest" CPU and attempt to pull tasks from it. This requires at least 2 CPUs, with the pinned-task CPU being overloaded relative to an idle or underloaded CPU.
- **Load balancing triggered**: Can be triggered by periodic load balancing (tick-based), newidle balancing (when a CPU goes idle), or nohz idle load balancing. The most common trigger is the periodic scheduler softirq (`SCHED_SOFTIRQ`).
- **No race condition required**: The bug is entirely deterministic given the right workload. Every load balance attempt that targets the overloaded CPU will exhibit the O(n) scan.

The probability of reproduction is very high (essentially guaranteed) given the workload — simply pin a large number of CFS tasks to one CPU while having another CPU idle. The hard lockup severity depends on the number of pinned tasks: with ~32 tasks, `loop_max` roughly matches `nr_running` so the effect is negligible; with 1000+ tasks, the lockup becomes observable; with 10,000+ tasks, it becomes a hard lockup.

## Reproduce Strategy (kSTEP)

This bug is reproducible with kSTEP. The strategy involves creating many CFS tasks pinned to a single CPU, then triggering load balancing from another CPU and observing the extended iteration in `detach_tasks()`.

### Step-by-step Plan

1. **Topology Setup**: Configure QEMU with at least 2 CPUs. Use 2 CPUs for simplicity (CPU 0 for the driver, CPU 1 as the pinned-task CPU, though CPU 0 will also serve as the "idle" destination CPU for load balancing). Actually, since CPU 0 is reserved for the driver, configure at least 3 CPUs: CPU 0 (driver), CPU 1 (will have pinned tasks), CPU 2 (idle destination for load balancing).

2. **Task Creation**: Create a large number of CFS tasks (e.g., 500–1000 tasks to keep it manageable while still exceeding `loop_max=32` by a wide margin). Pin all tasks to CPU 1 using `kstep_task_pin(p, 1, 2)` (restricting them to only CPU 1).

3. **Wake all tasks**: Use `kstep_task_wakeup(p)` for each task so they are all on CPU 1's runqueue and `nr_running` is large.

4. **Trigger load balancing**: Use `kstep_tick_repeat()` to advance ticks until the periodic load balancer fires. The load balancer should detect CPU 1 as overloaded and CPU 2 as idle, and attempt to pull tasks from CPU 1. Alternatively, use the `on_sched_balance_begin` or `on_sched_softirq_begin` callback to observe when load balancing starts.

5. **Observe the bug**: The key observation is the behavior of `detach_tasks()`. On the buggy kernel:
   - `detach_tasks()` will iterate through all tasks (well beyond `loop_max=32`) because `LBF_ALL_PINNED` is never cleared.
   - Use `KSYM_IMPORT` to access the `sched_nr_migrate` sysctl (or read `env.loop` / `env.loop_max` if accessible).
   - **Detection approach 1**: Use `on_sched_balance_begin` and `on_sched_softirq_end` callbacks to measure the time (or tick count) spent in load balancing. On the buggy kernel, this will be dramatically longer.
   - **Detection approach 2**: Add a `printk` or use `KSYM_IMPORT` to instrument `detach_tasks()` indirectly. Read `cpu_rq(1)->nr_running` before and after the load balance softirq — on both kernels, no tasks will be moved (all pinned), but the time spent will differ.
   - **Detection approach 3 (recommended)**: Set `sysctl_sched_nr_migrate` to a small value (e.g., 8) via `kstep_sysctl_write("sched_nr_migrate", "%d", 8)` and observe the `LBF_ALL_PINNED` flag behavior. After load balancing completes, check if the load balancer reported all-pinned (which triggers `env.flags & LBF_ALL_PINNED`-related logic in `sched_balance_rq()`). On the buggy kernel, the all-pinned path iterates through all tasks; on the fixed kernel, it stops after `loop_max`.

6. **Concrete detection method**: The most reliable approach is to measure the duration of the softirq. Use `ktime_get()` (via `KSYM_IMPORT` if needed) in `on_sched_softirq_begin` and `on_sched_softirq_end` to timestamp the softirq. With 500+ pinned tasks on the buggy kernel, the softirq will take noticeably longer than on the fixed kernel (where it stops after 32 iterations).

   Alternatively, instrument the load balancer more directly:
   - After load balancing completes on the buggy kernel, `env.flags & LBF_ALL_PINNED` is set, and `sd->nr_balance_failed` is incremented, eventually triggering active balancing. On both kernels this behavior is the same in terms of flags, but the *time* taken differs.
   - A simpler approach: use `kstep_sysctl_write("sched_nr_migrate", "%d", 4)` to set loop_max very small, create 200 pinned tasks, and use a timestamp around the softirq to see if the iteration time scales with task count (buggy) or is bounded (fixed).

7. **Pass/Fail Criteria**:
   - **Buggy kernel**: The time spent in the load balance softirq (or the number of loop iterations if observable) scales with the number of pinned tasks, significantly exceeding `loop_max`. The softirq takes a disproportionately long time.
   - **Fixed kernel**: The load balance softirq completes quickly, bounded by `loop_max` iterations regardless of total pinned task count.

8. **Alternative detection via `nr_balance_failed` timing**: On both kernels, load balancing will fail to move any tasks (all pinned). However, on the buggy kernel, each failed attempt takes O(n) time, while on the fixed kernel it takes O(loop_max) time. By running several load balance cycles and timing them, the difference should be clear.

9. **kSTEP capabilities needed**: The core kSTEP APIs are sufficient:
   - `kstep_task_create()` and `kstep_task_pin()` for creating pinned tasks
   - `kstep_task_wakeup()` to enqueue them
   - `kstep_tick()` / `kstep_tick_repeat()` to trigger periodic load balancing
   - `on_sched_softirq_begin` / `on_sched_softirq_end` callbacks for timing
   - `kstep_sysctl_write()` to configure `sched_nr_migrate`
   - `KSYM_IMPORT` for accessing `ktime_get` or scheduler internals
   - `kstep_pass()` / `kstep_fail()` for reporting results

   No additional kSTEP framework changes are required.

10. **Expected results**:
    - **Buggy kernel (pre-fix)**: Creating 500 tasks pinned to CPU 1, with CPU 2 idle, and triggering load balance will result in `detach_tasks()` scanning all 500 tasks. The softirq duration will be proportional to the task count. With enough tasks, this could even produce soft lockup warnings in the QEMU guest.
    - **Fixed kernel (post-fix)**: The same setup will result in `detach_tasks()` scanning at most `sysctl_sched_nr_migrate` tasks (default 32) before breaking out. The softirq completes in bounded time regardless of the number of pinned tasks.
