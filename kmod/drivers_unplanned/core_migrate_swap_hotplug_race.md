# Core: migrate_swap() vs. hotplug stopper wakeup race causes list corruption

**Commit:** `009836b4fa52f92cba33618e773b1094affa8cd2`
**Affected files:** kernel/sched/core.c, kernel/stop_machine.c
**Fixed in:** v6.16-rc5
**Buggy since:** v5.11-rc1 (commit `2558aacff858` "sched/hotplug: Ensure only per-cpu kthreads run during hotplug")

## Bug Description

A race condition exists between `migrate_swap()` (invoked from NUMA balancing) and CPU hotplug (`_cpu_down()`) that causes a double `list_add_tail` on the same `cpu_stop_work` structure, resulting in list corruption and a kernel BUG. The bug was discovered on MediaTek ARM64 systems running CPU hotplug stress tests, with an occurrence frequency of approximately once every 1–2 weeks.

The root cause is a confluence of two wakeup-deferral mechanisms — `wake_q` and `ttwu_queue_wakelist` — that together can delay the wakeup of a per-CPU stopper thread (`migration/N`) long enough for the `balance_push()` mechanism to fire a second time before the first push work has been consumed. The `balance_push()` machinery, introduced by commit `2558aacff858`, is part of the CPU hotplug sequence and is called from `schedule()` on a dying CPU to push non-per-cpu-kthread tasks off. It queues work to the stopper thread via `stop_one_cpu_nowait()`. If the stopper thread is not yet awake and visible to the scheduler, `schedule()` on the dying CPU re-enters `balance_push()` and attempts to queue the same `push_work` a second time — corrupting the linked list.

The bug manifests as a `list_add corruption` BUG in `lib/list_debug.c`, triggered from `cpu_stop_queue_work()` → `__cpu_stop_queue_work()` → `list_add_tail()`. The call trace shows it occurring inside `balance_push()` called from `__schedule()` → `preempt_schedule_common()`.

## Root Cause

The race involves three CPUs' worth of mechanisms, but fundamentally two CPUs: CPU0 performing `migrate_swap(cpu0, cpu1)` via `stop_two_cpus()`, and CPU1 undergoing hotplug-down via `_cpu_down()` → `sched_cpu_deactivate()`.

**Step 1: CPU1 begins hotplug-down.** `sched_cpu_deactivate()` calls `set_cpu_active(cpu1, false)` and `balance_push_set(cpu1, true)`. This arms the `balance_push()` callback for CPU1's `schedule()` path.

**Step 2: CPU0 queues stopper work for both CPUs.** `stop_two_cpus()` → `cpu_stop_queue_two_works()` calls `__cpu_stop_queue_work(stopper1, work1, &wakeq)` and `__cpu_stop_queue_work(stopper2, work2, &wakeq)`. Both stopper threads (`migration/0` and `migration/1`) are added to the local `wake_q`. Work is added to the stopper work lists. After releasing the locks, CPU0 calls `preempt_enable()` — at this point, **no wakeup has been issued yet** because `wake_up_q(&wakeq)` hasn't been called.

**Step 3: CPU1 enters schedule() and triggers balance_push().** Since CPU1 is dying and `balance_push` is armed, `schedule()` calls `balance_push()` → `stop_one_cpu_nowait()` → `cpu_stop_queue_work()`. Inside `cpu_stop_queue_work()`, `__cpu_stop_queue_work()` does `list_add_tail(&push_work->list, &stopper->works)` — this is the **1st add** of `push_work`. Then `wake_up_q(&wakeq)` is called, but `wakeq` is empty because `migration/1` was already added to CPU0's `wakeq`, not this one. The `wake_q_add()` uses `cmpxchg` on `wake_q_head->next` to prevent double-adds, so CPU1's local `wakeq` is indeed empty. **The stopper thread is NOT woken up.**

**Step 4: CPU0 finally calls `wake_up_q(&wakeq)`.** This wakes `migration/0` directly (same CPU, `ttwu_queue_cond` returns false because `cpu == smp_processor_id()`). Then it wakes `migration/1` — but `ttwu_queue_cond()` returns true for CPU1 because the CPUs don't share cache (or because the idle/nr_running heuristic triggers). This causes the wakeup to go through `ttwu_queue_wakelist()` → `__smp_call_single_queue()`, which sends an IPI to CPU1. The task is left in `TASK_WAKING` state — **not yet on CPU1's runqueue**.

**Step 5: CPU1 re-enters schedule() before the IPI is processed.** Since `migration/1` is in `TASK_WAKING` state and not on CPU1's runqueue, `pick_next_task()` does not see it. CPU1 picks another task, and when that task gets preempted or calls `schedule()`, `balance_push()` fires again. This time, `stop_one_cpu_nowait()` → `cpu_stop_queue_work()` → `__cpu_stop_queue_work()` does `list_add_tail(&push_work->list, ...)` **a second time** on the same `push_work` that is already on the list. This triggers the `list_add` corruption check in `lib/list_debug.c`.

**Step 6: Eventually CPU1 receives the IPI**, processes `sched_ttwu_pending()`, and wakes `migration/1` — but the damage is already done.

The critical insight is that both `wake_q` (which batches wakeups and defers them past lock release) and `ttwu_queue_wakelist` (which defers cross-CPU wakeups via IPI) can delay the stopper thread wakeup. For stopper threads, which must be woken immediately to prevent re-entrant `balance_push()`, these deferral mechanisms are fatal.

## Consequence

The immediate consequence is a kernel BUG (panic) caused by list corruption:

```
list_add corruption. prev->next should be next (ffffff82812c7a00),
but was 0000000000000000. (prev=ffffff82812c3208).
kernel BUG at lib/list_debug.c:34!
Call trace:
 __list_add_valid_or_report+0x11c/0x144
 cpu_stop_queue_work+0x440/0x474
 stop_one_cpu_nowait+0xe4/0x138
 balance_push+0x1f4/0x3e4
 __schedule+0x1adc/0x23bc
 preempt_schedule_common+0x68/0xd0
 preempt_schedule+0x60/0x80
 _raw_spin_unlock_irqrestore+0x9c/0xa0
```

The corrupted linked list in the stopper work queue means the kernel's stop_machine infrastructure is broken on that CPU. Even without `CONFIG_DEBUG_LIST`, the corruption could cause an infinite loop when iterating the work list, a use-after-free, or silent data corruption. In the worst case, this can lead to a complete system hang during CPU hotplug.

The bug is sporadic (roughly once every 1–2 weeks under stress testing) because it requires a very specific timing window: CPU0 must queue stopper works and release locks, then CPU1 must enter `schedule()` and trigger `balance_push()` before the IPI from CPU0's deferred wakeup arrives. This is inherently timing-dependent and more likely on systems with higher IPI latency or heavy interrupt load.

## Fix Summary

The fix addresses both wakeup-deferral mechanisms:

**Part 1: Remove `wake_q` from stop_machine.** The `__cpu_stop_queue_work()` function no longer takes a `wake_q_head` parameter and no longer calls `wake_q_add()`. Instead, `cpu_stop_queue_work()` calls `wake_up_process(stopper->thread)` directly after releasing the lock (guarded by the `enabled` check). Similarly, `cpu_stop_queue_two_works()` calls `wake_up_process()` directly for both stopper threads (guarded by `!err`). This eliminates the window where the stopper thread's wakeup is deferred by `wake_q` batching. Since there's only one stopper thread per CPU, the batching optimization of `wake_q` provides no benefit here.

**Part 2: Prevent ttwu_queue_cond() from deferring stopper wakeups.** A new check is added to `ttwu_queue_cond()` in `kernel/sched/core.c`:

```c
#ifdef CONFIG_SMP
    if (p->sched_class == &stop_sched_class)
        return false;
#endif
```

By returning `false`, `ttwu_queue_cond()` prevents stopper threads from being placed on the remote `wakelist`. This ensures `try_to_wake_up()` takes the direct path through `ttwu_do_activate()`, making the stopper thread immediately visible on the target runqueue. This eliminates the IPI-deferral window.

Together, these two changes ensure that stopper thread wakeups are always synchronous and immediate, closing the race window that allowed `balance_push()` to fire twice before the stopper thread consumed the first work item.

## Triggering Conditions

The bug requires the following precise conditions:

1. **NUMA balancing active**: `migrate_swap()` is called from `task_numa_migrate()` as part of NUMA balancing. This requires `CONFIG_NUMA_BALANCING=y`, at least 2 NUMA nodes, and tasks with memory access patterns that trigger NUMA page faults. The `migrate_swap()` call invokes `stop_two_cpus()`.

2. **Concurrent CPU hotplug**: A CPU must be in the process of going offline. Specifically, `sched_cpu_deactivate()` must have already called `set_cpu_active(cpu, false)` and `balance_push_set(cpu, true)`, but the stopper thread must not yet have run the push work. This is part of the `_cpu_down()` path.

3. **Timing window between lock release and wake_up_q**: On CPU0 (the `stop_two_cpus` caller), the window between `preempt_enable()` (after releasing stopper locks) and `wake_up_q(&wakeq)` must overlap with CPU1's `schedule()` path. During this window, CPU1 can enter `balance_push()`.

4. **wake_q already claimed**: The stopper thread on the dying CPU must already be on CPU0's `wake_q` (from `__cpu_stop_queue_work` in `cpu_stop_queue_two_works`). This means the `balance_push` path's `cpu_stop_queue_work` creates a local `wakeq` that is empty (because `wake_q_add` detects the thread is already on another queue), so no wakeup is issued from CPU1.

5. **ttwu_queue_cond returns true for the stopper**: When CPU0 finally calls `wake_up_q()` and wakes the dying CPU's stopper thread, `ttwu_queue_cond()` must return `true`, causing the wakeup to go through the IPI-based wakelist path instead of direct activation. This happens when CPUs are on different LLC domains or when the idle/nr_running heuristic triggers offloading.

6. **CPU1 re-enters schedule() before IPI delivery**: The IPI carrying the deferred wakeup must not arrive on CPU1 before CPU1 re-enters `schedule()` and triggers `balance_push()` a second time. This is a narrow timing window that depends on IPI latency and the workload on CPU1.

7. **At least 2 CPUs**: The system must have at least 2 CPUs. In practice, the bug was observed on 8-CPU ARM64 MediaTek systems. The probability increases with cross-NUMA configurations where `ttwu_queue_cond()` is more likely to return `true`.

The overall reproduction probability is very low (once every 1–2 weeks under dedicated hotplug stress testing) because all six conditions must be met simultaneously in a narrow timing window.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

**1. CPU hotplug is not supported.** The core triggering mechanism is `_cpu_down()` → `sched_cpu_deactivate()` → `balance_push_set()`. kSTEP has no API to simulate CPU hotplug events. There is no `kstep_cpu_offline()` or `kstep_cpu_hotplug()` function. The `balance_push()` mechanism is only armed during CPU hotplug teardown, and there is no way to trigger it from a kernel module without actually taking a CPU offline. kSTEP can configure topology (`kstep_topo_init/apply`) and CPU capacities/frequencies, but cannot dynamically bring CPUs online or offline.

**2. migrate_swap() requires real NUMA balancing.** The `migrate_swap()` function is called from `task_numa_migrate()` which is part of the NUMA balancing subsystem. It requires real userspace processes with `mm_struct` (to have NUMA page faults), real NUMA memory access latency patterns, and the autonomous NUMA balancing heuristics to decide a swap is beneficial. kSTEP's tasks are kernel-controlled and do not have userspace address spaces or memory access patterns. While `stop_two_cpus()` can theoretically be called from other contexts, the specific race requires `migrate_swap()` because it's the only caller that runs concurrently with hotplug without holding `cpu_hotplug_lock`.

**3. The race depends on wake_q and ttwu_wakelist timing.** The bug requires the IPI-based `ttwu_queue_wakelist` path to be taken for the stopper thread wakeup, which depends on cache topology (different LLC domains) and the scheduler's internal wakeup routing decisions. While kSTEP can set up topology, it cannot control the precise timing of IPI delivery or force `ttwu_queue_cond()` to take a specific path. The race window is measured in microseconds.

**4. What would need to be added to kSTEP:** To reproduce this bug, kSTEP would need:
   - A `kstep_cpu_hotplug(cpu, false)` API that triggers the full `_cpu_down()` → `sched_cpu_deactivate()` → `balance_push_set()` path while keeping the system functional. This is fundamentally complex because CPU hotplug involves many subsystems (interrupt controllers, timers, RCU, workqueues) and requires real hardware coordination.
   - A way to call `migrate_swap()` or `stop_two_cpus()` directly, bypassing the NUMA balancing subsystem. This would require a new kSTEP API like `kstep_stop_two_cpus(cpu1, cpu2, fn, arg)`.
   - Precise control over IPI delivery timing to create the necessary race window between stopper work queueing and stopper thread wakeup.

**5. Alternative reproduction methods:** The original reporters used a CPU hotplug stress test on real ARM64 hardware with NUMA balancing enabled. The test repeatedly onlines/offlines CPUs while running memory-intensive NUMA workloads. Even with this setup, reproduction takes 1–2 weeks. A more targeted approach might involve:
   - Using `ftrace` or `kprobes` to inject delays at strategic points (e.g., between `preempt_enable()` and `wake_up_q()` in `cpu_stop_queue_two_works()`).
   - Writing a kernel module that directly calls `stop_two_cpus()` in a loop while another thread triggers CPU hotplug via `/sys/devices/system/cpu/cpuN/online`.
   - Using `sched_setaffinity` and NUMA memory placement to ensure `ttwu_queue_cond()` takes the wakelist path.
