# Core: Missing set_rq_online() Rollback on Failed CPU Deactivation

**Commit:** `fe7a11c78d2a9bdb8b50afc278a31ac177000948`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.11-rc2
**Buggy since:** v5.11-rc1 (introduced by commit `120455c514f7` "sched: Fix hotplug vs CPU bandwidth control")

## Bug Description

When a CPU is being taken offline via the CPU hotplug state machine, the kernel calls `sched_cpu_deactivate()` early in the deactivation sequence. This function performs several preparatory steps, including marking the CPU's runqueue as offline via `sched_set_rq_offline()`. Later in the function, it calls `cpuset_cpu_inactive()` to update cpuset state. If `cpuset_cpu_inactive()` fails (e.g., because removing the CPU would cause SCHED_DEADLINE bandwidth to overflow), the error handling path is supposed to roll back all the changes made so far and return the system to its pre-deactivation state.

However, the error handling path in `sched_cpu_deactivate()` was missing a call to `sched_set_rq_online()` to undo the earlier `sched_set_rq_offline()`. While the error path correctly restored `cpu_active_mask`, `balance_push`, `sched_smt_present`, and NUMA state, it left the runqueue marked as offline (`rq->online = 0` and the CPU cleared from `rq->rd->online`). This meant the CPU was nominally active (present in `cpu_active_mask`) but its runqueue was in an offline state — an inconsistency that could cause scheduling anomalies.

The bug was introduced in commit `120455c514f7` ("sched: Fix hotplug vs CPU bandwidth control") which moved the `set_rq_offline()` call from `sched_cpu_dying()` (a point of no return) to `sched_cpu_deactivate()` (which can fail and roll back). The original placement in `sched_cpu_dying()` never needed rollback because by that point the CPU deactivation was irrevocable. Moving it earlier without adding the corresponding rollback created this defect.

This was discovered during stress testing by Yang Yingliang at Huawei, as part of a 4-patch series that also fixed a related unbalanced `sched_smt_present` dec/inc issue in the same error path.

## Root Cause

The function `sched_cpu_deactivate()` in `kernel/sched/core.c` performs the following sequence when deactivating a CPU:

1. `set_cpu_active(cpu, false)` — clears the CPU from `cpu_active_mask`
2. `balance_push_set(cpu, true)` — enables balance push to migrate tasks away
3. `synchronize_rcu()` — waits for RCU readers to finish
4. `sched_set_rq_offline(rq, cpu)` — marks `rq->online = 0` and clears the CPU from `rq->rd->online`, then calls `class->rq_offline(rq)` for each scheduling class (CFS, RT, DL)
5. `sched_smt_present_dec(cpu)` — decrements SMT present counter
6. `sched_core_cpu_deactivate(cpu)` — handles core scheduling deactivation
7. `sched_update_numa(cpu, false)` — updates NUMA state
8. `cpuset_cpu_inactive(cpu)` — updates cpuset state

If step 8 fails, the error path rolls back steps 7, 5, 2, and 1 in reverse:
```c
if (ret) {
    sched_smt_present_inc(cpu);    /* rollback step 5 */
    balance_push_set(cpu, false);   /* rollback step 2 */
    set_cpu_active(cpu, true);      /* rollback step 1 */
    sched_update_numa(cpu, true);   /* rollback step 7 */
    return ret;
}
```

**Step 4 (`sched_set_rq_offline`) was never rolled back.** The missing line is:
```c
sched_set_rq_online(rq, cpu);      /* rollback step 4 — MISSING */
```

The `sched_set_rq_offline()` function acquires the runqueue lock, checks that the root domain exists, and calls `set_rq_offline(rq)`, which does:
- Calls `update_rq_clock(rq)` to update the runqueue clock
- Iterates over all scheduling classes and calls `class->rq_offline(rq)` for each (this deregisters the runqueue from RT and DL push/pull infrastructure, among other things)
- Clears the CPU from `rq->rd->online` cpumask
- Sets `rq->online = 0`

Without the matching `sched_set_rq_online()` in the rollback, all of these changes persist even though the CPU is re-activated. The RT and DL push/pull mechanisms, which rely on `rq->online` and `rq->rd->online`, will no longer consider this CPU for task migration.

The `cpuset_cpu_inactive()` function can fail when `dl_bw_check_overflow(cpu)` returns `-EBUSY`, which happens when removing the CPU's capacity would cause the total SCHED_DEADLINE bandwidth to exceed what the remaining CPUs can provide.

## Consequence

The primary consequence is a state inconsistency where a CPU is marked active in `cpu_active_mask` but its runqueue is marked offline (`rq->online = 0`). This creates several problems:

**RT and DL scheduling breakage:** The RT scheduling class relies on `rq->online` for its push/pull task migration mechanism. When `rq_offline_rt()` is called (via `set_rq_offline`), it calls `__disable_runtime()` on the RT runqueue and invokes `pull_rt_task()` to migrate tasks away. If the runqueue remains offline, subsequent `rq_online_rt()` calls (which would re-enable runtime and register for push/pull) never happen. Similarly, the DL class calls `rq_offline_dl()` which dequeues the runqueue from the DL push/pull infrastructure. This means RT and DL tasks will not be properly migrated to or from this CPU, potentially causing priority inversions and deadline misses.

**General scheduling anomalies:** Other parts of the scheduler check `rq->online` or `rq->rd->online` cpumask when making scheduling decisions. For example, load balancing may skip this CPU during migration decisions, and `select_task_rq()` paths may avoid it. Since the CPU is still active and running tasks (it's in `cpu_active_mask`), any tasks that happen to run on it may get stranded there without proper load balancing. The `rq->rd->online` cpumask being stale with respect to `cpu_active_mask` can also cause inconsistent domain-level load balancing decisions.

**Cascading failures on subsequent hotplug:** If the same CPU is later successfully offlined and brought back online, the `set_rq_online(rq)` call in `sched_cpu_activate()` checks `if (!rq->online)` before acting, so it would correctly re-online the runqueue. However, intermediate states during these transitions could trigger BUG_ON checks or further inconsistencies.

## Fix Summary

The fix adds a single line — `sched_set_rq_online(rq, cpu)` — to the error handling path in `sched_cpu_deactivate()`, placed immediately before the existing `balance_push_set(cpu, false)` rollback. This ensures that if `cpuset_cpu_inactive()` fails, the runqueue is restored to its online state, matching the rollback of all other state changes.

The corrected error path now reads:
```c
if (ret) {
    sched_smt_present_inc(cpu);
    sched_set_rq_online(rq, cpu);   /* <-- added by the fix */
    balance_push_set(cpu, false);
    set_cpu_active(cpu, true);
    sched_update_numa(cpu, true);
    return ret;
}
```

The `sched_set_rq_online()` helper (introduced by the preceding patch 3/4 in the same series) acquires the runqueue lock, verifies the root domain exists, and calls `set_rq_online(rq)`, which restores `rq->online = 1`, re-adds the CPU to `rq->rd->online`, and calls `class->rq_online(rq)` for each scheduling class to re-register in push/pull infrastructure. This exactly reverses the effect of the earlier `sched_set_rq_offline()`.

The fix is correct and complete because it restores perfect symmetry in the error path: every state change made before the point of failure is now undone. The ordering of rollback operations is also correct — `sched_set_rq_online()` is called after `sched_smt_present_inc()` (reversing the forward order) and before `balance_push_set(cpu, false)`, ensuring the runqueue is online before balance push is disabled and the CPU is re-marked as active.

## Triggering Conditions

The bug requires a failed CPU hotplug deactivation, which needs the following specific conditions:

1. **CPU hotplug must be supported and initiated.** The system must support CPU hotplug (CONFIG_HOTPLUG_CPU), and something must initiate the deactivation of a CPU (e.g., writing `0` to `/sys/devices/system/cpu/cpuN/online` from userspace).

2. **`cpuset_cpu_inactive()` must fail.** This happens when `cpuhp_tasks_frozen` is false (i.e., we're not in a suspend/resume path) and `dl_bw_check_overflow(cpu)` returns `-EBUSY`. The overflow check verifies that removing the CPU's capacity from the root domain's DL bandwidth pool would not cause existing SCHED_DEADLINE reservations to exceed 100% of the remaining capacity. This means there must be SCHED_DEADLINE tasks with enough reserved bandwidth that removing one CPU tips the total over the limit.

3. **Specific SCHED_DEADLINE bandwidth configuration.** To trigger `dl_bw_check_overflow()` to return an error, the total DL bandwidth already reserved must be close to or at the capacity of the active CPUs. For example, with 4 CPUs and DL tasks reserving 90% of total capacity, removing one CPU would push utilization above 100%, causing the check to fail.

4. **Multi-CPU system.** At least 2 CPUs are required (one to deactivate, one to remain active). The scenario is more likely with more CPUs and DL tasks distributed across them.

5. **Timing.** The bug is triggered deterministically once `cpuset_cpu_inactive()` fails during `sched_cpu_deactivate()`. There is no race condition — it is a logic error in the error handling path. The probability depends only on whether the DL bandwidth overflow condition is met at the time of CPU deactivation.

The bug is observable after the failed deactivation: the CPU remains active but its runqueue is offline, leading to the scheduling anomalies described above. A subsequent successful hotplug cycle (offline + online) would fix the state, but the window between the failed offline and the next successful online cycle is problematic.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?** This bug fundamentally requires CPU hotplug — specifically, the ability to initiate a CPU deactivation that partially completes and then rolls back. The `sched_cpu_deactivate()` function is called by the CPU hotplug state machine (`cpuhp_invoke_callback()` from `cpuhp_thread_fun()`), which is a complex multi-step mechanism involving inter-CPU communication, smpboot thread management, RCU synchronization, and coordinated state transitions across multiple subsystems. kSTEP has no API to trigger CPU hotplug events. There is no `kstep_cpu_offline()` or equivalent function.

2. **Could we call `sched_cpu_deactivate()` directly via KSYM_IMPORT?** While theoretically possible, this would be unsafe and non-representative. `sched_cpu_deactivate()` expects to be called within the cpuhp state machine with specific preconditions met (cpuhp lock held, smpboot threads in the right state, etc.). Calling it ad-hoc from a kernel module would likely cause system crashes or hangs due to the `synchronize_rcu()` call, the `balance_push_set()` mechanism (which requires the cpuhp stopper thread infrastructure), and other dependencies on the hotplug state machine context. Even if it didn't crash, the test wouldn't faithfully reproduce the real-world bug scenario.

3. **Could we set up the failure condition?** To make `cpuset_cpu_inactive()` fail, we need `dl_bw_check_overflow()` to return `-EBUSY`. This requires SCHED_DEADLINE tasks with enough bandwidth to overflow when a CPU is removed. kSTEP does not have an API to create SCHED_DEADLINE tasks with specific runtime/period parameters (it has `kstep_task_fifo()` for RT but nothing for DL bandwidth admission). Even if we could create DL tasks, we still couldn't trigger the hotplug path.

4. **WHAT would need to be added to kSTEP?** Reproducing this bug would require:
   - A `kstep_cpu_hotplug(cpu, online)` API that triggers the full CPU hotplug state machine (not just calling individual functions). This would need to properly invoke the cpuhp callback chain, park/unpark smpboot threads, handle migration of tasks, and coordinate with all subsystems that register cpuhp callbacks.
   - A `kstep_task_deadline(p, runtime, period, deadline)` API to create SCHED_DEADLINE tasks with specific bandwidth parameters, enabling the DL bandwidth overflow condition.
   - These are not minor extensions — CPU hotplug simulation is a fundamental architectural capability that requires deep integration with the kernel's cpuhp infrastructure.

5. **Alternative reproduction methods outside kSTEP:**
   - **Bare-metal or VM with CPU hotplug:** The most straightforward approach is to use a kernel with CONFIG_HOTPLUG_CPU enabled and SCHED_DEADLINE tasks configured to consume near-maximum bandwidth. Then trigger CPU offline via sysfs: `echo 0 > /sys/devices/system/cpu/cpu1/online`. If DL bandwidth would overflow, the offline fails, and the bug is triggered.
   - **Stress test script:** Create several SCHED_DEADLINE tasks using `sched_setattr()` with runtime/period close to (N-1)/N of total capacity (where N is the number of CPUs). Then repeatedly offline random CPUs. Some attempts will fail due to DL bandwidth overflow, triggering the bug.
   - **Detection:** After a failed offline, read `rq->online` via debugfs or a kernel module. It should be 1 (online) for an active CPU. On the buggy kernel, it will be 0 (offline) despite the CPU being active. Alternatively, observe RT/DL task migration failures by checking if RT tasks on the affected CPU never get pulled away even when other CPUs are idle.
   - **QEMU/KVM with CPU hotplug:** QEMU supports CPU hotplug via the `device_add`/`device_del` monitor commands or vCPU hotplug. Run the kernel in QEMU with multiple vCPUs, set up DL tasks, and trigger CPU offline from inside the guest via sysfs. This provides a controlled environment for testing without kSTEP.
