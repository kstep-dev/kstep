# Deadline: dl_server hrtimer fires after CPU goes offline

**Commit:** `ee6e44dfe6e50b4a5df853d933a96bdff5309e6e`
**Affected files:** `kernel/sched/core.c`, `kernel/sched/deadline.c`
**Fixed in:** v6.18-rc2
**Buggy since:** v6.17 (introduced by commit `4ae8d9aa9f9d` "sched/deadline: Fix dl_server getting stuck")

## Bug Description

The dl_server is a per-CPU SCHED_DEADLINE entity that provides bandwidth guarantees for CFS (fair) tasks. Each CPU's runqueue has a `fair_server` (`struct sched_dl_entity`) that uses deadline scheduling mechanics — including hrtimers for budget replenishment and zero-laxity enforcement — to ensure fair tasks receive their share of CPU time even in the presence of RT tasks.

When a CPU is taken offline via hotplug (e.g., using `drmgr -c cpu -r -q 1` on PowerPC systems), the kernel goes through a teardown sequence that includes migrating tasks off the dying CPU and eventually calling `sched_cpu_dying()`. However, a race condition exists where the dl_server's hrtimer can be armed just before the CPU goes offline (triggered by `kthread_park` enqueuing a fair task on the dying CPU), and then fire after the CPU has been removed from the `cpu_present_mask`.

When this hrtimer fires, it calls `dl_server_timer()` → `enqueue_dl_entity()` → `cpudl_set()`, which contains a `WARN_ON(!cpu_present(cpu))` check. Since the CPU has already been removed from the present mask (by `drmgr` on PowerPC, which modifies `cpu_present_mask` after offlining), this triggers a kernel warning. The bug was reported by IBM CI on a Power9 system and was bisected to commit `4ae8d9aa9f9d` which made the dl_server more aggressively re-arm its timer, increasing the window for this race.

## Root Cause

The root cause is a missing cleanup step in the CPU hotplug teardown path. When a CPU is dying, `sched_cpu_dying()` in `kernel/sched/core.c` is called as part of the `CPUHP_AP_SCHED_STARTING` teardown stage. Before the fix, this function did not stop the dl_server for the dying CPU. The sequence of events that triggers the bug is:

1. A CPU hotplug removal operation begins (e.g., `drmgr -c cpu -r -q 1` on PowerPC).
2. During the teardown, `kthread_park()` is called to park per-CPU kernel threads. This operation enqueues a fair task on the dying CPU's runqueue.
3. When a fair task is enqueued on an empty runqueue, `dl_server_start()` is called, which sets `dl_se->dl_server_active = 1`, calls `enqueue_dl_entity()`, and arms the dl_server's hrtimer (`dl_timer`). This timer is typically set for the zero-laxity moment (the last possible moment where the server can still complete its budget before its deadline).
4. The CPU proceeds through the hotplug teardown. On PowerPC with `drmgr`, the CPU is removed from `cpu_present_mask` after going offline.
5. The dl_server's hrtimer fires (via `__hrtimer_run_queues()` → `hrtimer_interrupt()` → `timer_interrupt()`). This calls `dl_server_timer()`.
6. Inside `dl_server_timer()`, after replenishing the dl entity's runtime budget, `enqueue_dl_entity()` is called, which eventually calls `cpudl_set()`.
7. `cpudl_set()` contains `WARN_ON(!cpu_present(cpu))`, which triggers because the CPU has been removed from `cpu_present_mask`.

The critical issue is that commit `4ae8d9aa9f9d` ("sched/deadline: Fix dl_server getting stuck") removed the `server_has_tasks()` check in `dl_server_timer()`. Previously, if there were no fair tasks when the timer fired, the timer would simply call `replenish_dl_entity()` and `dl_server_stopped()` and return `HRTIMER_NORESTART` without re-enqueueing. After `4ae8d9aa9f9d`, the timer always proceeds to enqueue the dl entity, which means it will attempt to call `cpudl_set()` even on a dead/non-present CPU.

Additionally, there was no guard in `dl_server_start()` to prevent starting the dl_server on an already-offline CPU, and `sched_cpu_dying()` did not explicitly stop the dl_server before the CPU was marked dead.

## Consequence

The immediate consequence is a kernel `WARNING` splat at `kernel/sched/cpudeadline.c:219` (`cpudl_set+0x58/0x170`). The warning fires because `cpudl_set()` asserts `WARN_ON(!cpu_present(cpu))` — the CPU has been removed from the present mask but the dl_server is still attempting to update the CPU's deadline in the `cpudl` data structure.

The full call trace is:
```
WARNING: CPU: 0 PID: 0 at kernel/sched/cpudeadline.c:219 cpudl_set+0x58/0x170
  cpudl_set+0x58/0x170
  dl_server_timer+0x168/0x2a0
  __hrtimer_run_queues+0x1a4/0x390
  hrtimer_interrupt+0x124/0x300
  timer_interrupt+0x140/0x320
```

Beyond the warning, the dl_server operating on a non-present CPU could corrupt the `cpudl` heap data structure, since it indexes by CPU number and the CPU's position in the deadline-ordered heap may be invalid. This could lead to incorrect deadline task placement decisions on subsequent CPU hotplug operations. The warning was observed firing multiple times in succession on the same system, indicating the timer kept re-arming. On systems where `cpu_present_mask` is not modified during hotplug (e.g., x86, which never modifies `cpu_present_mask` after boot), the WARN would not trigger, but the dl_server would still be running unnecessarily on a dead CPU.

## Fix Summary

The fix adds two changes:

**1. Stop dl_server in `sched_cpu_dying()` (`kernel/sched/core.c`):** The function now explicitly calls `dl_server_stop(&rq->fair_server)` while holding the runqueue lock, just before releasing the lock. This dequeues the dl entity, cancels any pending hrtimer (`hrtimer_try_to_cancel(&dl_se->dl_timer)`), clears the `dl_defer_armed`, `dl_throttled`, and `dl_server_active` flags. An `update_rq_clock(rq)` call is also added before this to ensure the runqueue clock is up to date for the dequeue operation. This ensures that when the CPU is marked dead, its dl_server is fully stopped and no timer can fire.

**2. Guard `dl_server_start()` (`kernel/sched/deadline.c`):** A `WARN_ON_ONCE(!cpu_online(cpu_of(rq)))` check is added at the beginning of `dl_server_start()`. If the CPU is not online, the function returns immediately without starting the dl_server. This prevents the dl_server from being (re-)started on a CPU that is in the process of going offline, closing the race window where `kthread_park` could start the dl_server after the CPU has begun teardown. The `WARN_ON_ONCE` serves as a diagnostic to catch any future callers that attempt this.

These two changes together are belt-and-suspenders: the guard in `dl_server_start()` prevents the timer from being armed in the first place on an offline CPU, and the stop in `sched_cpu_dying()` ensures any already-armed timer is cancelled during the final teardown.

## Triggering Conditions

The following precise conditions are required to trigger the bug:

- **Kernel version:** Linux v6.17 or later (after commit `4ae8d9aa9f9d` was merged), up to but not including the fix in v6.18-rc2.
- **CPU hotplug:** The system must support CPU hotplug. The bug is triggered during a CPU removal operation. On PowerPC, this is done via `drmgr -c cpu -r -q 1`. On x86, regular `echo 0 > /sys/devices/system/cpu/cpuN/online` could potentially trigger it, though the `WARN_ON(!cpu_present(cpu))` in `cpudl_set()` would not fire on x86 since x86 never modifies `cpu_present_mask` after boot.
- **Timing window:** A fair task must be enqueued on the dying CPU's runqueue after the CPU has begun its hotplug teardown but before `sched_cpu_dying()` is called. This typically happens when `kthread_park()` enqueues a per-CPU kthread. The dl_server's hrtimer must then fire after the CPU is fully dead.
- **dl_server enabled:** The dl_server must be active (it is enabled by default on modern kernels with `CONFIG_SMP`). The `fair_server` entity must have non-zero `dl_runtime`.
- **cpu_present_mask modification (for WARN):** To trigger the specific `WARN_ON(!cpu_present(cpu))` in `cpudl_set()`, the platform must remove the CPU from `cpu_present_mask` during hotplug. This happens on PowerPC with `drmgr` but not with plain `chcpu`. On x86, the present mask is not modified, so the WARN would not fire, but the underlying issue (dl_server running on a dead CPU) still exists.

The race window is relatively narrow but reliably reproducible on PowerPC with `drmgr`. Shrikanth Hegde from IBM confirmed they could reproduce it consistently on their test systems.

## Reproduce Strategy (kSTEP)

### Why this bug cannot be reproduced with kSTEP

1. **Requires CPU hotplug:** The fundamental trigger for this bug is the CPU hotplug teardown sequence — specifically, the `sched_cpu_dying()` code path that is called when a CPU transitions through the `CPUHP_AP_SCHED_STARTING` teardown state. kSTEP does not have any CPU hotplug API. There is no `kstep_cpu_offline()` or equivalent function that can trigger the CPU hotplug state machine.

2. **Requires real hotplug state machine:** The CPU hotplug process involves dozens of kernel subsystems registering callbacks at various states (e.g., `CPUHP_AP_SCHED_STARTING`, `CPUHP_TEARDOWN_CPU`, etc.). The dl_server gets started during this teardown because `kthread_park()` — which is called as part of parking per-CPU kernel threads — enqueues a fair task. Simulating this would require running the full CPU hotplug state machine, which is deeply integrated with architecture-specific code (interrupt controllers, timer infrastructure, etc.).

3. **Requires hrtimer firing on dead CPU:** The bug manifests when an hrtimer fires on a CPU that has transitioned past the point of no return in its teardown. In kSTEP, timers are advanced via `kstep_tick()`, but this simulates a regular scheduler tick, not the low-level hrtimer infrastructure that fires on a dying CPU's residual interrupt context.

4. **Requires cpu_present_mask modification:** The observable symptom (the WARN) requires the CPU to be removed from `cpu_present_mask`, which is a platform-specific operation (PowerPC `drmgr` does this, x86 does not). kSTEP has no mechanism to modify CPU present/online masks.

### What would need to be added to kSTEP

To support this class of bugs, kSTEP would need:

- **A `kstep_cpu_hotplug(cpu, online/offline)` API** that triggers the full CPU hotplug state machine for a given CPU. This would need to call into `cpu_down()` / `cpu_up()` kernel functions, which involve taking CPUs through all registered hotplug states, migrating tasks, stopping per-CPU kthreads, and eventually calling `sched_cpu_dying()`.
- **QEMU CPU hotplug support** configured in the VM, since the CPU must actually be removable at the hardware level for the hotplug to succeed.
- **Ability to modify CPU masks** (present, online, possible) to simulate platform-specific behaviors like PowerPC's `drmgr`.

These are fundamental architectural changes, not minor extensions, because CPU hotplug involves the entire kernel's hotplug infrastructure, not just the scheduler.

### Alternative reproduction methods

The bug can be reproduced outside kSTEP by:

1. **On PowerPC:** Run `drmgr -c cpu -r -q 1` on a system running Linux v6.17 with commit `4ae8d9aa9f9d` applied but without the fix `ee6e44dfe6e50b4a5df853d933a96bdff5309e6e`. The warning should appear in `dmesg`.
2. **On x86 or any SMP system:** Write a script that repeatedly offlines and onlines CPUs via `/sys/devices/system/cpu/cpuN/online`. While the WARN in `cpudl_set()` may not fire (since x86 doesn't modify `cpu_present_mask`), adding a `WARN_ON(!cpu_online(cpu))` to `dl_server_start()` (as Peter Zijlstra suggested in the mailing list thread) would confirm the dl_server is being started on an offline CPU.
3. **Kernel debugging:** Enable `CONFIG_PROVE_LOCKING` and `CONFIG_DEBUG_PREEMPT` for additional diagnostics. Adding `ftrace` tracepoints on `dl_server_start`, `dl_server_stop`, `dl_server_timer`, and `sched_cpu_dying` would help visualize the race.
