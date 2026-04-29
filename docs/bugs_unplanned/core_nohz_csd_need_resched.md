# Core: Spurious need_resched() check in nohz_csd_func skips idle load balancing

**Commit:** `ea9cffc0a154124821531991d5afdd7e8b20d7aa`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.13-rc3
**Buggy since:** v5.8-rc1 (commit b2a02fc43a1f "smp: Optimize send_call_function_single_ipi()")

## Bug Description

The `nohz_csd_func()` function in `kernel/sched/core.c` is the SMP call function
handler that runs on an idle CPU when it is selected as the idle load balancer (ILB)
target. Its job is to raise `SCHED_SOFTIRQ` so the idle CPU performs load balancing
on behalf of other nohz-idle CPUs. Before raising the softirq, it checks two
conditions: (1) `idle_cpu(cpu)` confirms the CPU is still idle, and (2)
`!need_resched()` confirms there is no pending reschedule.

The `!need_resched()` check became semantically incorrect after commit b2a02fc43a1f
("smp: Optimize send_call_function_single_ipi()"), introduced in v5.8. That commit
optimized IPI delivery to idle CPUs that have `TIF_POLLING_NRFLAG` set: instead of
sending a real hardware IPI, the sender simply sets `TIF_NEED_RESCHED` on the
polling idle CPU's thread_info, causing it to fall out of the idle polling loop and
process queued SMP call functions via `flush_smp_call_function_queue()` on the
idle-exit path. This means `TIF_NEED_RESCHED` is now overloaded — it no longer
exclusively indicates an impending task wakeup; it can also mean "there's a queued
SMP call function for you to process."

On x86 systems using `mwait_idle_with_hints()` (via the `intel_idle` driver),
`TIF_POLLING_NRFLAG` is set during nohz idle. When a busy CPU kicks the idle load
balancer via `kick_ilb()` → `smp_call_function_single_async()`, the
`call_function_single_prep_ipi()` function detects `TIF_POLLING_NRFLAG` via
`set_nr_if_polling()` and sets `TIF_NEED_RESCHED` instead of sending a real IPI.
The idle CPU exits its polling loop, calls `flush_smp_call_function_queue()`, which
invokes `nohz_csd_func()`. At this point, `need_resched()` returns true — not
because there's a task to schedule, but because `TIF_NEED_RESCHED` was set by the
IPI optimization. The `!need_resched()` check in `nohz_csd_func()` therefore fails,
and `SCHED_SOFTIRQ` is never raised, causing the idle load balance to be entirely
skipped.

## Root Cause

The root cause is the semantic mismatch between the meaning of `TIF_NEED_RESCHED`
and how `nohz_csd_func()` interprets it.

Before commit b2a02fc43a1f, `TIF_NEED_RESCHED` was only set when a genuine
reschedule was needed — typically after a task wakeup that preempts the current
task. The `!need_resched()` check in `nohz_csd_func()` was originally meaningful:
it detected cases where a task had been woken on the CPU between the ILB kick and
the execution of `nohz_csd_func()`. If `need_resched()` was true, it meant a task
was already running or about to run, so idle load balancing was unnecessary.

After b2a02fc43a1f, `send_call_function_single_ipi()` was changed to:
```c
static __always_inline void
send_call_function_single_ipi(int cpu)
{
    if (call_function_single_prep_ipi(cpu)) {
        arch_send_call_function_single_ipi(cpu);
    }
}
```

Where `call_function_single_prep_ipi()` calls `set_nr_if_polling(cpu_rq(cpu)->idle)`.
If the idle CPU has `TIF_POLLING_NRFLAG` set, `set_nr_if_polling()` atomically sets
`TIF_NEED_RESCHED` on the idle thread and returns true, meaning no real IPI is needed
— the polling idle loop will detect `need_resched()` and exit. The function then
returns `false`, skipping the actual IPI.

The execution flow on the idle CPU after wakeup is:
1. `do_idle()` loop: `while (!need_resched())` exits because `TIF_NEED_RESCHED` is set
2. `preempt_set_need_resched()` propagates the flag
3. `tick_nohz_idle_exit()` handles tick housekeeping
4. `__current_clr_polling()` clears `TIF_POLLING_NRFLAG`
5. `flush_smp_call_function_queue()` processes queued call functions, including `nohz_csd_func()`
6. Inside `nohz_csd_func()`: `idle_cpu(cpu)` returns true (no tasks, no ttwu_pending),
   but `need_resched()` ALSO returns true (set by the IPI optimization, NOT by a task wakeup)
7. The check `if (rq->idle_balance && !need_resched())` evaluates to false
8. `SCHED_SOFTIRQ` is NOT raised → idle load balance is skipped entirely

The `idle_cpu()` check alone is sufficient to detect all cases where the CPU is no
longer truly idle. The function checks `rq->curr != rq->idle`, `rq->nr_running != 0`,
and `rq->ttwu_pending != 0`. Any legitimate task wakeup racing with the ILB kick
would set either `rq->ttwu_pending` (for remote wakeups) or increment `rq->nr_running`
(for local wakeups), both of which cause `idle_cpu()` to return false. The
`need_resched()` check is therefore redundant and harmful.

## Consequence

The consequence is that idle load balancing is effectively disabled on systems where
idle CPUs use `TIF_POLLING_NRFLAG`-based idle (primarily x86 systems with the
`intel_idle` driver using `mwait_idle_with_hints()`). This includes most modern Intel
and AMD server and desktop systems.

When idle load balancing is skipped, tasks pile up on busy CPUs while other CPUs
remain idle. This causes poor CPU utilization, increased scheduling latency, and
reduced throughput for multi-threaded workloads. The cover letter for the patch series
mentions that the issue was particularly noticed on Intel Ice Lake Xeon servers
(2×32C/64T) and AMD systems. Julia Lawall reported a significant reduction in load
balancing attempts at the NUMA level, which was root-caused to a combination of this
bug and commit 3dcac251b066 ("sched/core: Introduce SM_IDLE and an idle re-entry
fast-path in __schedule()").

The bug does not cause crashes or hangs, but it degrades scheduling fairness and
performance. On large multi-socket systems, it can lead to significant load imbalance,
with some CPUs at 100% utilization while neighboring CPUs sit idle. Benchmark results
from the patch series showed measurable improvement (0.26%–0.89% throughput increase
on bt.B.x OMP benchmark) after the fix, though the real-world impact depends on
workload characteristics and system topology.

## Fix Summary

The fix is a single-line change in `nohz_csd_func()` that removes the `!need_resched()`
check from the condition to raise `SCHED_SOFTIRQ`:

```c
// Before (buggy):
if (rq->idle_balance && !need_resched()) {
    rq->nohz_idle_balance = flags;
    raise_softirq_irqoff(SCHED_SOFTIRQ);
}

// After (fixed):
if (rq->idle_balance) {
    rq->nohz_idle_balance = flags;
    raise_softirq_irqoff(SCHED_SOFTIRQ);
}
```

The fix is correct because `idle_cpu()` (which sets `rq->idle_balance` on the
preceding line) already comprehensively checks whether the CPU is truly idle. It
checks `rq->curr != rq->idle` (a task is running), `rq->nr_running != 0` (tasks are
queued), and `rq->ttwu_pending != 0` (a remote wakeup is pending). If any of these
conditions is true, `idle_cpu()` returns false and `rq->idle_balance` is set to 0,
preventing the softirq from being raised. There is no additional information that
`need_resched()` provides that isn't already captured by `idle_cpu()`.

The commit message exhaustively analyzes all possible race conditions between task
wakeups and idle load balancing, demonstrating that `idle_cpu()` alone is sufficient
in every case: a remote wakeup via `ttwu_queue_cond()` sets `rq->ttwu_pending`
before queuing the wake function, and a local wakeup via `ttwu_do_activate()`
increments `rq->nr_running` before calling `wakeup_preempt()` which might set
`TIF_NEED_RESCHED`. In both cases, the state change that matters (`ttwu_pending` or
`nr_running`) happens before `TIF_NEED_RESCHED` is set, so `idle_cpu()` catches it
first.

## Triggering Conditions

The bug requires the following precise conditions:

1. **TIF_POLLING_NRFLAG idle**: The target idle CPU must have `TIF_POLLING_NRFLAG`
   set in its thread_info when it is in the nohz (tickless) idle state. This occurs
   on x86 systems using the `intel_idle` driver with `mwait_idle_with_hints()`, or
   the `acpi_idle` driver with FFH (MWAIT-based) C-states. The `mwait` instruction
   monitors a memory address while keeping `TIF_POLLING_NRFLAG` set, allowing the
   IPI optimization to apply.

2. **CONFIG_NO_HZ_COMMON=y**: The kernel must be configured with nohz support so
   that idle CPUs can enter tickless mode and be managed by the nohz idle load
   balancer.

3. **Multiple CPUs**: At least 2 CPUs are needed — one busy CPU to trigger
   `nohz_balancer_kick()` and one idle CPU to be the ILB target.

4. **Load imbalance**: The busy CPU must have `rq->nr_running >= 2` (or meet other
   conditions in `nohz_balancer_kick()` like `check_cpu_capacity()`,
   `sched_asym()`, or `check_misfit_status()`) so it decides to kick the ILB.

5. **SMP call function queuing**: `kick_ilb()` must find the idle CPU via
   `find_new_ilb()` and queue `nohz_csd_func` via `smp_call_function_single_async()`.
   The `call_function_single_prep_ipi()` call inside
   `send_call_function_single_ipi()` must detect `TIF_POLLING_NRFLAG` on the idle
   CPU and set `TIF_NEED_RESCHED` instead of sending a real IPI.

6. **No concurrent task wakeup on the idle CPU**: The idle CPU must remain truly
   idle (no tasks woken up on it) between the ILB kick and the execution of
   `nohz_csd_func()`. If a task were woken up, `idle_cpu()` would return false and
   the `need_resched()` check wouldn't matter.

The bug is highly reliable on affected hardware: on x86 systems using `intel_idle`
with mwait, essentially every ILB kick to a polling-idle CPU will fail. The cover
letter describes this as happening "in most instances" on x86 with
`mwait_idle_with_hints()`.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Why it cannot be reproduced

The bug fundamentally requires `TIF_POLLING_NRFLAG` to be set on the idle CPU's
thread_info **while the CPU is in nohz (tickless) idle**. This is a
hardware-dependent CPU idle behavior:

- **On real x86 hardware**: The `intel_idle` driver uses `mwait_idle_with_hints()`
  which keeps `TIF_POLLING_NRFLAG` set while the CPU monitors a memory address
  using the MWAIT instruction. The CPU remains in this polling-like state with the
  tick stopped (nohz idle). When another CPU queues an SMP call function,
  `call_function_single_prep_ipi()` detects `TIF_POLLING_NRFLAG` and sets
  `TIF_NEED_RESCHED` instead of sending a real IPI.

- **In QEMU TCG (kSTEP's execution environment)**: The default idle path uses the
  `hlt` instruction via `default_idle_call()`. Crucially, `default_idle_call()`
  calls `current_clr_polling_and_test()` which **clears** `TIF_POLLING_NRFLAG`
  before entering `hlt`. When the CPU is actually sleeping in `hlt`,
  `TIF_POLLING_NRFLAG` is NOT set. Therefore, `call_function_single_prep_ipi()`
  finds `TIF_POLLING_NRFLAG` clear and sends a **real hardware IPI** instead of
  using the set-TIF_NEED_RESCHED optimization. With a real IPI, `TIF_NEED_RESCHED`
  is not spuriously set, and `nohz_csd_func()` works correctly — the bug does not
  manifest.

- **The `idle=poll` kernel parameter** is not a viable workaround. While `idle=poll`
  keeps `TIF_POLLING_NRFLAG` set (via `cpu_idle_poll()`), it also calls
  `tick_nohz_idle_restart_tick()` inside `do_idle()`, which **restarts the tick**.
  With the tick running, the CPU does regular periodic load balancing via
  `scheduler_tick()` → `trigger_load_balance()` → `raise_softirq(SCHED_SOFTIRQ)`.
  It is never selected for nohz idle load balancing because it exits the nohz idle
  mask. The nohz ILB code path that contains the bug is never exercised.

- **Forcing cpuidle polling state** (by disabling all cpuidle states except state 0)
  is also unreliable. The cpuidle polling state (`poll_idle()`) does set
  `TIF_POLLING_NRFLAG`, but the cpuidle governor's `stop_tick` decision and the
  interaction with nohz tick management make it uncertain whether the tick would
  actually be stopped. Additionally, QEMU TCG may not have a properly configured
  cpuidle subsystem — without real ACPI C-state support, the cpuidle framework
  may fall back to `default_idle_call()` entirely, bypassing cpuidle states.

### 2. What would need to be added to kSTEP

To reproduce this bug, kSTEP would need one of the following fundamental changes:

- **Real mwait-based idle support**: Run QEMU with KVM (not TCG) on a host that
  supports MWAIT passthrough, so the guest kernel's `intel_idle` driver properly
  uses mwait and keeps `TIF_POLLING_NRFLAG` set during nohz idle. This is a
  fundamental architectural change from TCG to KVM, which affects determinism and
  reproducibility guarantees.

- **A kSTEP API to simulate the TIF_POLLING_NRFLAG IPI optimization**: Something
  like `kstep_simulate_polling_ipi(cpu)` that directly sets `TIF_NEED_RESCHED` on
  an idle CPU's thread_info before `nohz_csd_func()` runs. This would require
  hooking into the SMP call function delivery path or the nohz_csd_func execution
  path, which is a significant kernel-level instrumentation change. It would also
  constitute directly manipulating internal scheduler state.

- **A hook in `call_function_single_prep_ipi()`**: To force the polling optimization
  path regardless of actual `TIF_POLLING_NRFLAG` state. This would require modifying
  core SMP infrastructure code, which is outside the scheduler subsystem that kSTEP
  instruments.

### 3. Alternative reproduction methods

Outside kSTEP, the bug can be reproduced on real x86 hardware:

1. Use a system with an Intel CPU that supports the `intel_idle` driver (most modern
   Intel and AMD systems).
2. Boot a kernel between v5.8 and v6.13-rc2 with `CONFIG_NO_HZ_COMMON=y`.
3. Run a workload that creates load imbalance — e.g., multiple CPU-bound tasks
   pinned to a single CPU while other CPUs are idle.
4. Trace the nohz idle load balancer with `trace-cmd` or ftrace, filtering on
   `sched_wake_idle_without_ipi` and `SCHED_SOFTIRQ` events.
5. On the buggy kernel, observe that `nohz_csd_func` is called but `SCHED_SOFTIRQ`
   is rarely raised (because `need_resched()` returns true in most cases on x86
   mwait-based idle).
6. On the fixed kernel, observe that `SCHED_SOFTIRQ` is reliably raised after
   `nohz_csd_func`.

Alternatively, the impact can be observed indirectly by running multi-threaded
benchmarks (e.g., NAS Parallel Benchmarks bt.B.x) and comparing throughput between
buggy and fixed kernels on x86 hardware with mwait idle.
