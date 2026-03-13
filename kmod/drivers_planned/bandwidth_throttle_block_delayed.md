# Bandwidth: Delayed Task Not Blocked on Throttled Hierarchy Dequeue

**Commit:** `e67e3e738f088e6c5ccfab618a29318a3f08db41`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.12 stable (stable-only patch; upstream indirectly fixed in v6.18 by `e1fad12dcb66`)
**Buggy since:** `b7ca5743a260` ("sched/core: Tweak wait_task_inactive() to force dequeue sched_delayed tasks"), merged in v6.16-rc1, backported to stable v6.12+

## Bug Description

When a CFS task with delayed dequeue (`sched_delayed = true`) resides in a cgroup whose CFS bandwidth has been exhausted (the `cfs_rq` is throttled), force-dequeuing that task via `dequeue_entities()` with the `DEQUEUE_DELAYED` flag fails to call `__block_task()`. This leaves the task's `p->on_rq` at 1 even though the task has been removed from the scheduler's runqueue hierarchy. The task is effectively in limbo: it is not on any runqueue (so it can never be picked to run or properly blocked), but `task_on_rq_queued()` still returns true.

This inconsistency was introduced by commit `b7ca5743a260`, which added a code path in `wait_task_inactive()` to force-dequeue `sched_delayed` tasks by calling `dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_DELAYED)`. Before that commit, the missing `__block_task()` call in the throttled hierarchy path was mostly harmless: other callers that dequeued delayed tasks would check `task_on_rq_queued()` and re-enqueue if it returned true, and the task would eventually be properly blocked when picked after the hierarchy was unthrottled.

However, `wait_task_inactive()` is fundamentally different from those callers. It expects the dequeue to transition the task to the blocked state (`p->on_rq = 0`) so that `task_on_rq_queued()` returns false. When the dequeue returns early due to a throttled hierarchy without calling `__block_task()`, `wait_task_inactive()` still sees the task as queued and loops back to retry — creating an infinite loop. The task is now detached from the hierarchy, so even unthrottling the `cfs_rq` will never cause the task to be picked and properly blocked.

The real-world trigger reported by Cloudflare involved a Kubernetes workload where a thread group was being killed (coredump path). Task A in `do_coredump()` → `zap_threads()` called `wait_task_inactive()` on Task B, which was in `do_exit()` → `schedule()` in a sched_delayed state within a bandwidth-throttled cgroup. This caused Task A to spin forever and Task B to trigger RCU stalls.

## Root Cause

The root cause is in the `dequeue_entities()` function in `kernel/sched/fair.c`. This function walks up the scheduler entity hierarchy dequeuing entities from their respective `cfs_rq`s. There are two `cfs_rq_throttled()` checks — one in the first `for_each_sched_entity` loop (after dequeuing and updating `h_nr_*` accounting) and one in the second loop (which handles the "break" case where a parent still has other entities). Both checks previously contained `return 0` statements that exited the function immediately.

The critical code at the end of `dequeue_entities()` is:

```c
if (p && task_delayed) {
    SCHED_WARN_ON(!task_sleep);
    SCHED_WARN_ON(p->on_rq != 1);
    hrtick_update(rq);
    __block_task(rq, p);  /* Sets p->on_rq = 0 */
}
return 1;
```

When `dequeue_entities()` is called with `DEQUEUE_DELAYED` for a task whose `cfs_rq` hierarchy is throttled, the following sequence occurs:

1. `dequeue_entity(cfs_rq, se, flags)` successfully removes the task's scheduling entity from its local `cfs_rq` (even though the `cfs_rq` is throttled, the entity is removed from the local rbtree).
2. The `h_nr_*` counters on the task's `cfs_rq` are decremented.
3. Walking up to the parent entity, the code encounters `cfs_rq_throttled(cfs_rq)` returning true.
4. The old code executed `return 0`, completely bypassing the `__block_task()` call at the bottom of the function.
5. The task's `p->on_rq` remains at 1, and `task_on_rq_queued(p)` continues to return true.

The reason the early return existed is that when a `cfs_rq` is throttled, its parent `cfs_rq` has already been detached from the hierarchy by the throttle path. Therefore, there is no need to continue walking up the hierarchy adjusting `h_nr_*` accounting or calling `sub_nr_running()`. However, the early return was too aggressive — it also skipped the `__block_task()` call that is essential for marking delayed-dequeued tasks as no longer queued.

The `__block_task()` function performs the critical state transition:
- Sets `p->on_rq = 0` (via `WRITE_ONCE(p->on_rq, 0)`)
- Calls `deactivate_task()` accounting
- Makes `task_on_rq_queued(p)` return false

Without this call, the task is in an inconsistent state: removed from the scheduler data structures but still reported as queued.

## Consequence

The primary consequence is an **infinite loop** in `wait_task_inactive()`. When one task calls `wait_task_inactive()` on a sched_delayed task that is in a throttled CFS bandwidth hierarchy:

1. `wait_task_inactive()` acquires the rq lock, sees `p->se.sched_delayed == true`.
2. It calls `dequeue_task(rq, p, DEQUEUE_SLEEP | DEQUEUE_DELAYED)` to force-dequeue the task.
3. `dequeue_entities()` hits the throttled `cfs_rq` and returns 0 without calling `__block_task()`.
4. `wait_task_inactive()` checks `task_on_rq_queued(p)` which returns true (since `p->on_rq == 1`).
5. It enters the "queued" branch, sets a 1-tick timeout, releases the rq lock, and loops back.
6. On the next iteration, the state is unchanged — the task is still reported as queued.
7. This loop continues forever since the task is detached from the hierarchy and can never be picked to run.

The observable symptoms reported by Cloudflare were:

- **Task A (coredumper) spins forever** in `wait_task_inactive()`, consuming 100% of a CPU core.
- **Task B (exiting thread) triggers RCU stalls** because it is stuck in `do_exit()` → `schedule()` and never completes its exit, preventing RCU grace period advancement.
- The RCU stall trace shows Task B in `__schedule()` → `schedule()` → `do_exit()` → `do_group_exit()` → `get_signal()` call chain.
- The system becomes progressively unstable due to RCU stalls, potentially leading to a full system hang.

This bug only manifests on systems using CFS bandwidth throttling (typically Kubernetes/container environments with CPU limits) running kernels v6.12+ with both delayed dequeue and the `b7ca5743a260` patch applied.

## Fix Summary

The fix replaces the two `return 0` statements in the `cfs_rq_throttled()` checks within `dequeue_entities()` with `goto out` statements. A new label `out:` is placed just before the existing `if (p && task_delayed)` block, and a new local variable `int ret = 0` is introduced to track the return value.

The key changes are:

1. **`int ret = 0;`** — initialized to 0 (the throttled return value). This variable tracks whether the dequeue completed fully (1) or was stopped at a throttled hierarchy (0).

2. **`if (cfs_rq_throttled(cfs_rq)) goto out;`** (two occurrences) — instead of returning immediately, control jumps to the `out` label which falls through to the `__block_task()` code.

3. **`ret = 1;`** placed just before the `out:` label — so if we reach this point via the normal (non-throttled) path, `ret` is set to 1. If we arrived via `goto out` from a throttled check, `ret` remains 0.

4. **`return ret;`** at the end — replaces the old `return 1`.

The effect is that `__block_task()` is now called for delayed tasks regardless of whether the dequeue was stopped at a throttled hierarchy. This ensures `p->on_rq` is set to 0 and `task_on_rq_queued()` returns false, allowing `wait_task_inactive()` to see the task as properly blocked and exit its loop.

As noted in the commit message, this fix is a stable-only patch. Upstream v6.18 indirectly fixes the issue via commit `e1fad12dcb66` ("sched/fair: Switch to task based throttle model"), which completely removes the early `return 0` for throttled hierarchies in `dequeue_entities()` as part of the per-task throttle feature.

## Triggering Conditions

The following conditions must all be met simultaneously:

1. **Kernel version**: v6.12 or newer with delayed dequeue support (`CONFIG_SCHED_EEVDF` effectively, which is the default), AND commit `b7ca5743a260` present (v6.16-rc1 or backported to stable).

2. **CFS bandwidth throttling enabled**: `CONFIG_CFS_BANDWIDTH=y` (enabled by default when `CONFIG_CGROUPS` and `CONFIG_FAIR_GROUP_SCHED` are set). A cgroup must have a CPU bandwidth limit set (e.g., `cpu.max` in cgroupv2 with a quota less than the period).

3. **A task in a sched_delayed state**: A CFS task within the bandwidth-limited cgroup must have gone to sleep (DEQUEUE_SLEEP) and had its dequeue delayed by the EEVDF delayed dequeue mechanism. This means the task's `se.sched_delayed = true` and `p->on_rq = 1` (still reported as queued even though it has logically stopped running).

4. **The cgroup's CFS bandwidth quota must be exhausted**: The `cfs_rq` associated with the task's cgroup must be in a throttled state (`cfs_rq->throttle_count > 0`). This happens when the cgroup's runtime is consumed within a period.

5. **Another task calls `wait_task_inactive()` on the sched_delayed task**: In the real-world case, this happens during signal delivery for fatal signals (coredump path: `do_coredump()` → `zap_threads()` → `wait_task_inactive()`). It can also happen via `kthread_bind()` → `__kthread_bind_mask()` → `wait_task_inactive()`.

6. **Timing**: The sched_delayed state and the throttled state must overlap. The task must first go to sleep (becoming sched_delayed), and the cfs_rq must be throttled when `wait_task_inactive()` tries to force-dequeue it with `DEQUEUE_DELAYED`. The throttling must have already occurred (so the hierarchy above is detached), but the task must still be in the local `cfs_rq`.

The probability of hitting this in production is **moderate to high** in containerized environments (Kubernetes clusters with CPU limits). Cloudflare reported hitting it reliably on their production Kubernetes nodes. The bug is deterministic once the conditions are met — it is not a race condition. The key requirement is simply having a sched_delayed task inside a bandwidth-throttled cgroup when someone calls `wait_task_inactive()` on it.

The minimum CPU count is 1 (the bug is not topology-dependent), but practical reproduction requires at least 2 CPUs so that the task calling `wait_task_inactive()` runs on a different CPU than the target task.

## Reproduce Strategy (kSTEP)

The bug can be reproduced in kSTEP using CFS bandwidth throttling (already supported via `kstep_cgroup_write()` with `cpu.max`) and direct manipulation of the dequeue path. The strategy is to set up a sched_delayed task in a throttled hierarchy, then perform a force-dequeue with `DEQUEUE_DELAYED` and check whether `p->on_rq` transitions to 0.

### Step-by-step plan:

**1. Topology and task setup:**
- Configure QEMU with at least 2 CPUs.
- Create one CFS task (`target_task`) that will become the sched_delayed task in the throttled hierarchy.
- Create another CFS task (`helper_task`) to help consume bandwidth.
- Pin both tasks to CPU 1 (not CPU 0, which is reserved for the driver).

**2. Cgroup and bandwidth configuration:**
- Create a cgroup: `kstep_cgroup_create("bw_grp")`
- Set a very small CPU bandwidth quota: `kstep_cgroup_write("bw_grp", "cpu.max", "1000 100000")` (1ms per 100ms period = 1% bandwidth).
- Add `target_task` and `helper_task` to the cgroup: `kstep_cgroup_add_task("bw_grp", target_task->pid)`, same for `helper_task`.

**3. Wake tasks and consume bandwidth:**
- Wake both tasks: `kstep_task_wakeup(target_task)`, `kstep_task_wakeup(helper_task)`.
- Tick until the cgroup's `cfs_rq` becomes throttled. Use `kstep_tick()` in a loop, checking `cfs_rq_throttled(cfs_rq)` where `cfs_rq = target_task->sched_task_group->cfs_rq[1]` (CPU 1's cfs_rq for this task group).
- Approximately 1–2 ticks should be enough if the tick interval is set to 1ms (`step_interval_us = 1000`).

**4. Make the target task sched_delayed:**
- After the cfs_rq is throttled, block the target task: `kstep_task_block(target_task)`.
- The task should enter the sched_delayed state. Verify: `target_task->se.sched_delayed == true` and `target_task->on_rq == 1`.
- If the task is not in the sched_delayed state (because the cfs_rq is throttled and delay dequeue doesn't apply in this specific state), an alternative approach is to block the task BEFORE the cfs_rq is throttled (so it enters sched_delayed), then tick until the cfs_rq is throttled with the task already in sched_delayed state. The ordering would be:
  - Wake `target_task` and `helper_task`.
  - Tick a few times (not enough to exhaust quota).
  - Block `target_task` → it becomes sched_delayed.
  - Continue ticking (with `helper_task` consuming bandwidth) until `cfs_rq` is throttled.

**5. Perform force-dequeue and check state (the bug check):**
- Import `dequeue_task`: `KSYM_IMPORT_TYPED(dequeue_task_type, dequeue_task)` where `dequeue_task_type` is `bool (*)(struct rq *, struct task_struct *, int)`.
- Acquire the rq lock for CPU 1: `struct rq *rq = cpu_rq(1); raw_spin_rq_lock(rq);`
- Verify preconditions: `target_task->se.sched_delayed == true`, `target_task->on_rq == 1`, and `cfs_rq_throttled(cfs_rq) == true`.
- Call: `KSYM_dequeue_task(rq, target_task, DEQUEUE_SLEEP | DEQUEUE_DELAYED);`
- Check the result: `target_task->on_rq` should be 0 after a successful dequeue+block.
- Release the rq lock: `raw_spin_rq_unlock(rq);`

**6. Pass/fail criteria:**
- On the **buggy kernel**: After `dequeue_task()`, `target_task->on_rq == 1` (bug: `__block_task()` was not called). Report: `kstep_fail("on_rq still 1 after force dequeue on throttled hierarchy")`.
- On the **fixed kernel**: After `dequeue_task()`, `target_task->on_rq == 0` (fix: `__block_task()` was called via the `goto out` path). Report: `kstep_pass("on_rq correctly set to 0 after force dequeue")`.

**7. Callbacks:**
- No special callbacks (`on_tick_begin`, etc.) are needed. The driver can be entirely sequential in the `run()` function.

**8. Driver configuration:**
- `step_interval_us = 1000` (1ms tick interval, matching the default HZ=1000 configuration).
- `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)` guard (delayed dequeue was introduced in v6.12).

**9. Alternative approach (detecting the hang):**
If directly checking `p->on_rq` is insufficient or if you want to also verify the `wait_task_inactive()` behavior end-to-end, an alternative is to use `kstep_kthread_create()` and `kstep_kthread_bind()`. Create a kthread, place it in the bandwidth-limited cgroup, let it sleep (sched_delayed) while the cfs_rq is throttled, then attempt `kstep_kthread_bind()` from the driver context. On the buggy kernel, `kthread_bind()` → `wait_task_inactive()` would loop forever, which could be detected via a timeout mechanism (e.g., run the bind in a separate kthread and check if it completes within a reasonable number of ticks). However, this approach risks hanging the entire VM on the buggy kernel, so the direct state check in approach 5-6 is preferred.

**10. Key internal symbols and structures needed:**
- `cfs_rq_throttled()` — available via `internal.h` (includes `kernel/sched/sched.h`).
- `dequeue_task` — imported via `KSYM_IMPORT_TYPED`.
- `cpu_rq()`, `raw_spin_rq_lock()`/`raw_spin_rq_unlock()` — available via `internal.h`.
- `DEQUEUE_SLEEP`, `DEQUEUE_DELAYED` — defined in `kernel/sched/sched.h`, available via `internal.h`.
- `target_task->se.sched_delayed`, `target_task->on_rq` — direct struct member access.
- `target_task->sched_task_group->cfs_rq[cpu]` — to get the per-CPU cfs_rq for the task's cgroup.
