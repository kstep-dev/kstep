# Deadline: dl_server Proxy Tasks Double-Count nr_running

**Commit:** `52d15521eb75f9b521744db675bee61025d2fa52`
**Affected files:** kernel/sched/deadline.c
**Fixed in:** v6.17-rc4
**Buggy since:** v6.8-rc1 (commit `63ba8422f876` "sched/deadline: Introduce deadline servers")

## Bug Description

When deadline servers were introduced in v6.8, they created a mechanism for SCHED_DEADLINE entities to act as proxies for lower-priority scheduling classes (primarily CFS/SCHED_OTHER). The "fair server" (`rq->fair_server`) is a `sched_dl_entity` that wraps the CFS runqueue, providing a deadline scheduling bandwidth guarantee to prevent CFS task starvation when higher-priority RT tasks monopolize the CPU. The fair server is enqueued into the DL runqueue whenever CFS tasks transition from an empty to non-empty state on a given CPU.

The bug is that when a dl_server proxy entity is enqueued or dequeued via `inc_dl_tasks()` / `dec_dl_tasks()` in `kernel/sched/deadline.c`, these functions unconditionally call `add_nr_running()` / `sub_nr_running()`. This is incorrect because the dl_server is not a real runnable task — it is a proxy that runs on behalf of other tasks that are already counted in `rq->nr_running` by their own scheduling class's enqueue path. The result is that `rq->nr_running` is incremented twice for a single actual task: once by the deadline class for the dl_server proxy, and once by the fair class for the actual CFS task.

This double-counting causes `rq->nr_running` to report 2 when only 1 task is actually runnable on the CPU. While the inflated count may cause subtle scheduling anomalies in general, it has a catastrophic effect during CPU hotplug: the `balance_hotplug_wait()` function waits for `rq->nr_running == 1` (meaning only the hotplug control thread remains), but with the double-count, this condition is never satisfied, causing an infinite hang.

The bug was observed on an ARM64 system running kernel 6.15.0-rc4+ during CPU offline operations. The kernel stalled with "blocked for more than 120 seconds" messages for kworker threads involved in CPU hotplug (`work_for_cpu_fn`) and dependent workqueues (`vmstat_shepherd`).

## Root Cause

The root cause lies in the `inc_dl_tasks()` and `dec_dl_tasks()` functions in `kernel/sched/deadline.c`. Before the fix, these functions unconditionally called `add_nr_running()` and `sub_nr_running()` for every `sched_dl_entity` being enqueued or dequeued from the DL runqueue:

```c
void inc_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
    u64 deadline = dl_se->deadline;
    dl_rq->dl_nr_running++;
    add_nr_running(rq_of_dl_rq(dl_rq), 1);  // BUG: also counts dl_server proxies
    inc_dl_deadline(dl_rq, deadline);
}
```

The problem is that dl_server entities (identified by `dl_se->dl_server == 1`) are proxy entities, not real tasks. When a CFS task is enqueued to an idle CPU, the following call chain executes inside `enqueue_task_fair()`:

1. The for-each loop enqueues the CFS sched_entity, incrementing `cfs_rq->h_nr_queued`.
2. The condition `if (!rq_h_nr_queued && rq->cfs.h_nr_queued)` detects the CFS runqueue transitioning from empty to non-empty.
3. `dl_server_start(&rq->fair_server)` is called, which calls `enqueue_dl_entity()` → `__enqueue_dl_entity()` → `inc_dl_tasks()` → `add_nr_running(rq, 1)`. Now `rq->nr_running` becomes 1.
4. After the dl_server_start call, `enqueue_task_fair()` itself calls `add_nr_running(rq, 1)`. Now `rq->nr_running` becomes 2.

The symmetric problem exists on dequeue: `dec_dl_tasks()` calls `sub_nr_running()` for the dl_server, and `dequeue_task_fair()` also calls `sub_nr_running()` for the actual task. This means the count goes down by 2 as well, but the interim inflated value of `rq->nr_running` is the source of the hang.

The `dl_server()` inline function simply checks the `dl_server` field of the `sched_dl_entity`:

```c
static bool dl_server(struct sched_dl_entity *dl_se)
{
    return dl_se->dl_server;
}
```

This field is set to 1 for proxy entities like `rq->fair_server` during `dl_server_init()` and remains 0 for regular `SCHED_DEADLINE` tasks.

The critical interaction with CPU hotplug occurs in `balance_hotplug_wait()` in `kernel/sched/core.c`:

```c
static void balance_hotplug_wait(void)
{
    struct rq *rq = this_rq();
    rcuwait_wait_event(&rq->hotplug_wait,
                       rq->nr_running == 1 && !rq_has_pinned_tasks(rq),
                       TASK_UNINTERRUPTIBLE);
}
```

During CPU offline, the hotplug control thread (`cpuhp`) migrates all tasks off the dying CPU except itself, then calls `balance_hotplug_wait()` to wait until it is the only remaining task (`nr_running == 1`). With the double-counting bug, even when `cpuhp` is the sole task, `rq->nr_running` is 2 (1 for the task + 1 for the dl_server proxy), and the wait condition never becomes true.

## Consequence

The primary consequence is a **complete CPU hotplug hang**. When a CPU is taken offline, the `cpuhp` thread on the dying CPU enters `balance_hotplug_wait()` and spins indefinitely in `TASK_UNINTERRUPTIBLE` state, waiting for `rq->nr_running == 1`. Since `rq->nr_running` is inflated to 2 by the dl_server proxy counting, this condition is never met. The CPU offline operation never completes.

This hang has cascading effects. The CPU hotplug lock (`cpus_read_lock`) is held by the thread initiating the CPU offline operation (`work_for_cpu_fn` workqueue). Any other kernel subsystem that requires `cpus_read_lock` — such as `vmstat_shepherd`, which periodically collects per-CPU VM statistics — also blocks indefinitely. The kernel reports "blocked for more than 120 seconds" warnings with stack traces showing tasks stuck in `percpu_rwsem_wait()` (for `cpus_read_lock`) and `wait_for_completion()` (for `cpuhp_kick_ap_work`). The full stack traces from the bug report show:

- `vmstat_shepherd` (kworker/0:1, pid 11) blocked in `cpus_read_lock()` → `percpu_rwsem_wait()`
- `work_for_cpu_fn` (kworker/1:1, pid 116) blocked in `cpuhp_kick_ap_work()` → `wait_for_completion()`

Beyond the hotplug hang, the inflated `rq->nr_running` can cause more subtle issues in normal operation: incorrect load balancing decisions (the CPU appears busier than it is), incorrect idle detection, and potentially wrong behavior in any code path that checks `rq->nr_running` for scheduling decisions. However, the hotplug hang is the most severe and deterministic consequence.

## Fix Summary

The fix adds a simple guard in both `inc_dl_tasks()` and `dec_dl_tasks()` to skip the `add_nr_running()` / `sub_nr_running()` calls when the `sched_dl_entity` is a dl_server proxy:

```c
void inc_dl_tasks(struct sched_dl_entity *dl_se, struct dl_rq *dl_rq)
{
    u64 deadline = dl_se->deadline;
    dl_rq->dl_nr_running++;

    if (!dl_server(dl_se))
        add_nr_running(rq_of_dl_rq(dl_rq), 1);

    inc_dl_deadline(dl_rq, deadline);
}
```

The same pattern is applied to `dec_dl_tasks()` for `sub_nr_running()`. The `dl_rq->dl_nr_running` counter (which tracks DL-class entities specifically, including servers) is still incremented/decremented unconditionally — only the runqueue-wide `rq->nr_running` is guarded.

This fix is correct because the dl_server is a scheduling abstraction, not an actual runnable task. It provides bandwidth guarantees for CFS tasks by wrapping them in a deadline entity, but the underlying CFS task is already counted by `enqueue_task_fair()` → `add_nr_running()`. Counting the dl_server proxy additionally would mean a single task contributes 2 to `rq->nr_running`, which violates the invariant that `rq->nr_running` equals the number of actually runnable tasks. The fix restores this invariant by ensuring only real deadline tasks (not proxy servers) affect `rq->nr_running`.

## Triggering Conditions

The bug is triggered whenever a CFS task is enqueued onto a CPU whose CFS runqueue was previously empty (`h_nr_queued == 0`), causing the fair server's `dl_server_start()` to activate. The specific conditions are:

1. **Kernel version**: v6.8 or later (when deadline servers were introduced), up to v6.17-rc3 (before the fix). The kernel must have `CONFIG_SCHED_DEADLINE=y` (enabled by default in most configurations).

2. **Fair server active**: The `rq->fair_server` must have non-zero `dl_runtime`. This is the default configuration — the fair server is initialized during `sched_init()` with runtime=950000ns and deadline=1000000ns (95% CPU bandwidth). Unless the fair server has been explicitly disabled or its runtime set to 0, it will be active.

3. **CFS runqueue goes from empty to non-empty**: A CFS task must be enqueued on a CPU that had no CFS tasks. This is extremely common — it happens every time a CFS task wakes up on an idle CPU. The condition in `enqueue_task_fair()` is `if (!rq_h_nr_queued && rq->cfs.h_nr_queued)`.

4. **For the hotplug hang specifically**: A CPU offline operation must be in progress on the affected CPU. The `cpuhp` thread calls `balance_hotplug_wait()` which checks `rq->nr_running == 1`. With the double-count, this condition fails. This is triggered by writing to `/sys/devices/system/cpu/cpuN/online` or any operation that calls `cpu_down()`.

The bug is highly deterministic — it occurs every time a CFS task is enqueued to an idle CPU on an affected kernel. The double-counting of `nr_running` is 100% reproducible. The hotplug hang is also deterministic: it occurs every time a CPU offline is attempted on an affected kernel, since the `cpuhp` thread enqueuing itself on the dying CPU will trigger the dl_server start path.

No race conditions are involved — this is a straightforward logic error in the accounting code path. No special hardware, NUMA topology, or cgroup configuration is required.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP by observing the incorrect `rq->nr_running` value after enqueuing a single CFS task onto an idle CPU. While the most dramatic consequence (CPU hotplug hang) requires actual CPU hotplug operations, the underlying accounting bug — `rq->nr_running` being 2 when only 1 task is runnable — is directly observable through kSTEP's internal access capabilities.

**Step-by-step kSTEP driver plan:**

1. **Topology setup**: Configure QEMU with at least 2 CPUs (CPU 0 for the driver, CPU 1 as the test CPU). No special topology or NUMA configuration is needed.

2. **Version guard**: Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)` since the deadline server feature (and thus the bug) was introduced in v6.8.

3. **Create a CFS task pinned to CPU 1**: Use `kstep_task_create()` to create a CFS task, then `kstep_task_pin(p, 1, 2)` to pin it to CPU 1. Do not start it yet, or immediately block it with `kstep_task_block(p)`.

4. **Ensure CPU 1's CFS runqueue is empty**: Block the task (or don't wake it yet) so CPU 1 has no CFS tasks and is idle. Use `cpu_rq(1)->cfs.h_nr_queued` to verify it is 0.

5. **Record `rq->nr_running` before enqueue**: Use `KSYM_IMPORT` or direct access to read `cpu_rq(1)->nr_running` and verify it is 0 (or whatever baseline exists for the idle CPU — it should be 0 if no other tasks are pinned there).

6. **Wake the task**: Call `kstep_task_wakeup(p)` to enqueue the CFS task onto CPU 1. This triggers `enqueue_task_fair()` → `dl_server_start()` → `inc_dl_tasks()` → the buggy `add_nr_running()`, followed by `enqueue_task_fair()`'s own `add_nr_running()`.

7. **Check `rq->nr_running` immediately after wakeup**: In a tick callback (`on_tick_begin`) or immediately after the wakeup call, read `cpu_rq(1)->nr_running`.

8. **Pass/fail criteria**:
   - **Buggy kernel** (v6.8 to v6.17-rc3): `rq->nr_running == 2` for a single CFS task. Call `kstep_fail("nr_running double-counted: %d", nr_running)`.
   - **Fixed kernel** (v6.17-rc4+): `rq->nr_running == 1`. Call `kstep_pass("nr_running correct: %d", nr_running)`.

9. **Additional verification**: Also read `cpu_rq(1)->dl.dl_nr_running` to verify the dl_server is actually enqueued (should be 1 on both buggy and fixed kernels, since the dl_server is always tracked in `dl_nr_running` — only `rq->nr_running` is affected). This confirms the dl_server started and the test conditions are valid.

10. **Cleanup**: Block the task again, verify nr_running goes back to 0. On buggy kernels, nr_running would go from 2 → 0 (sub by 2), while on fixed kernels it goes from 1 → 0 (sub by 1). Either way the final state should be 0.

**Key implementation details:**

- The driver should use `kstep_sleep()` or a few `kstep_tick()` calls after waking the task to ensure the enqueue path has fully completed before reading `nr_running`.
- Use `struct rq *rq = cpu_rq(1);` via kSTEP's internal.h access to read `rq->nr_running` and `rq->dl.dl_nr_running`.
- The fair server (`rq->fair_server`) should be active by default on v6.8+ kernels. If not, the test can verify `rq->fair_server.dl_runtime != 0` as a precondition and skip if the fair server is inactive.
- No cgroup configuration is needed. No special sysctl writes are needed. The default kernel configuration with `CONFIG_SCHED_DEADLINE=y` is sufficient.
- The test is fully deterministic: the double-counting happens every time a CFS task is enqueued to an idle CFS runqueue, with no timing dependencies or race conditions.

**Alternative detection method**: Instead of (or in addition to) directly reading `nr_running`, the driver could create a second observation: block/wake the task repeatedly and log `nr_running` on each transition, building a trace that clearly shows the +2/-2 pattern on buggy kernels vs +1/-1 on fixed kernels. This provides stronger evidence and rules out transient measurement artifacts.
