# LB: Idle Load Balancer Aborts Due to Spurious need_resched from IPI Optimization

**Commit:** `ff47a0acfcce309cf9e175149c75614491953c8f`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.13-rc3
**Buggy since:** `b2a02fc43a1f` ("smp: Optimize send_call_function_single_ipi()"), merged in v5.8-rc1

## Bug Description

The idle load balancer (ILB) in the Linux scheduler can spuriously abort its work of balancing load across idle NOHZ CPUs due to a false positive from the `need_resched()` check in `_nohz_idle_balance()`. This check was designed to detect when the ILB CPU itself has received new work (e.g., a task was woken up on it), signaling that it should stop doing load balancing on behalf of other idle CPUs and attend to its own task. However, commit `b2a02fc43a1f` introduced an IPI optimization that overloads `TIF_NEED_RESCHED` with a new meaning, causing the check to trigger even when no real task wakeup has occurred.

The optimization in commit `b2a02fc43a1f` targets idle CPUs that are in `TIF_POLLING_NRFLAG` mode — a power-saving idle state where the CPU continuously polls the `TIF_NEED_RESCHED` flag instead of halting. When another CPU wants to send an IPI (e.g., via `send_call_function_single_ipi()`) to such a polling idle CPU, it avoids the expensive hardware IPI by simply setting the `TIF_NEED_RESCHED` flag on the idle task's `thread_info`. This causes the idle CPU to exit its polling loop and process the queued SMP call function via `flush_smp_call_function_queue()` in the idle exit path.

The problem arises because this `TIF_NEED_RESCHED` flag, set by the IPI optimization, is not cleared until `schedule_idle()` eventually calls `__schedule()`. Between the flag being set and its clearing, any code checking `need_resched()` — including the idle load balancer — will see it as true, even though no actual task has been woken up on the CPU. Specifically, when the scheduler triggers idle load balancing by queuing `nohz_csd_func()` on an idle CPU, the IPI optimization sets `TIF_NEED_RESCHED`, the CPU exits idle, `flush_smp_call_function_queue()` runs, `nohz_csd_func()` raises `SCHED_SOFTIRQ`, and `do_softirq_post_smp_call_flush()` processes the softirq. At this point, `_nohz_idle_balance()` runs with `TIF_NEED_RESCHED` still set, sees `need_resched()` returning true, and aborts the load balancing loop.

This is particularly prevalent on x86 systems where `mwait_idle_with_hints()` sets `TIF_POLLING_NRFLAG` for long idle periods, but also affects any configuration where CPUs use poll-based idle (e.g., `idle=poll` kernel parameter or `cpu_idle_poll()`).

## Root Cause

The root cause lies in the `_nohz_idle_balance()` function in `kernel/sched/fair.c`. This function iterates over all NOHZ idle CPUs performing load balancing on their behalf. Inside the main loop (`for_each_cpu_wrap(balance_cpu, nohz.idle_cpus_mask, this_cpu+1)`), there is a check meant to abort the loop if the ILB CPU itself has received work:

```c
/* In the buggy version: */
if (need_resched()) {
    if (flags & NOHZ_STATS_KICK)
        has_blocked_load = true;
    if (flags & NOHZ_NEXT_KICK)
        WRITE_ONCE(nohz.needs_update, 1);
    goto abort;
}
```

The intent of this check is to detect that a new task has been woken up on `this_cpu` (the ILB CPU) while it is processing load balancing for other CPUs. If such a wakeup occurs, the ILB CPU should stop doing work for others and schedule the newly woken task promptly. The `goto abort` skips the remainder of the loop and jumps to cleanup code that does not perform the final self-balance.

The IPI optimization from `b2a02fc43a1f` breaks this assumption. The optimization is in `send_call_function_single_ipi()` in `kernel/smp.c`:

```c
void send_call_function_single_ipi(int cpu)
{
    struct rq *rq = cpu_rq(cpu);
    if (!set_nr_if_polling(rq->idle))
        arch_send_call_function_single_ipi(cpu);
    else
        trace_ipi_send_cpumask(...);
}
```

When the target CPU's idle task has `TIF_POLLING_NRFLAG` set, `set_nr_if_polling()` atomically sets `TIF_NEED_RESCHED` on that task's `thread_info` and returns true, avoiding the real IPI. The polling idle loop in `cpu_idle_poll()` or `mwait_idle_with_hints()` detects the flag and exits, eventually calling `flush_smp_call_function_queue()`.

The critical timing sequence that triggers the bug is:

1. CPU A detects load imbalance and decides to trigger idle load balancing on idle CPU B (the ILB CPU), which is in `TIF_POLLING_NRFLAG` mode.
2. CPU A queues `nohz_csd_func()` on CPU B via `smp_call_function_single_async()`, which internally calls `send_call_function_single_ipi()`.
3. Instead of a hardware IPI, `set_nr_if_polling()` sets `TIF_NEED_RESCHED` on CPU B's idle task.
4. CPU B exits its idle polling loop due to the `TIF_NEED_RESCHED` flag.
5. `do_idle()` calls `flush_smp_call_function_queue()`, which processes the queued `nohz_csd_func()`.
6. `nohz_csd_func()` raises `SCHED_SOFTIRQ` to trigger idle load balancing.
7. `do_softirq_post_smp_call_flush()` processes the `SCHED_SOFTIRQ`, calling `sched_balance_softirq()` → `nohz_idle_balance()` → `_nohz_idle_balance()`.
8. Inside `_nohz_idle_balance()`, the code enters the `for_each_cpu_wrap` loop. On the very first (or any subsequent) iteration, it hits `if (need_resched())`.
9. `need_resched()` returns true because `TIF_NEED_RESCHED` was set at step 3 and has NOT been cleared yet — `__schedule()` has not run since the flag was set.
10. The code jumps to `abort`, skipping all load balancing work for the remaining idle CPUs.

The `TIF_NEED_RESCHED` flag is only cleared later when `schedule_idle()` calls `__schedule()` → `__schedule_loop()` → `prepare_task()` which calls `clear_tsk_need_resched()`. But `_nohz_idle_balance()` runs in softirq context before `schedule_idle()` is reached.

## Consequence

The primary consequence is **degraded load balancing responsiveness across idle CPUs**. When the ILB aborts prematurely, tasks that should be migrated from overloaded CPUs to idle CPUs are not migrated, leading to:

1. **Persistent load imbalance**: Tasks pile up on a few CPUs while others remain idle. The imbalance persists until the next ILB attempt succeeds (which requires a new ILB kick where the conditions don't trigger the spurious bail-out), or until a newidle balance on an idle CPU happens to pull tasks.

2. **Performance degradation**: On systems with many cores (servers, NUMA machines), the ILB is critical for distributing work. When it repeatedly bails out early, throughput suffers because available CPU capacity goes unused. K Prateek Nayak measured this on a dual-socket Intel Ice Lake Xeon server (2×32C/64T) and observed up to ~0.89% improvement in NAS bt.B.x benchmark throughput after fixing the bug, confirming the load balancing was being impaired.

3. **Increased latency for blocked load updates**: When the ILB aborts, it also sets `has_blocked_load = true` and `nohz.needs_update = 1`, signaling that blocked load statistics were not fully updated. This means stale blocked load data persists longer, potentially causing incorrect decisions in subsequent load balancing attempts.

The bug is particularly impactful on x86 systems using `mwait` for idle (common on Intel/AMD server CPUs) because `mwait_idle_with_hints()` always sets `TIF_POLLING_NRFLAG`, making the IPI optimization trigger frequently. Every ILB kick on such a system goes through the optimized path, and the ILB will bail out on the first `need_resched()` check in `_nohz_idle_balance()`. This makes idle load balancing effectively non-functional on these systems whenever the IPI optimization is used to trigger the ILB.

Julia Lawall independently reported reduced load balancing activity on NUMA-level scheduling domains (on a v6.11-based kernel), which was partially attributed to this interaction. The bug is a latent performance problem rather than a crash or functional failure — the system continues to run but with suboptimal task placement.

## Fix Summary

The fix adds a `!idle_cpu(this_cpu)` check before the existing `need_resched()` check in `_nohz_idle_balance()`:

```c
/* Fixed version: */
if (!idle_cpu(this_cpu) && need_resched()) {
    if (flags & NOHZ_STATS_KICK)
        has_blocked_load = true;
    if (flags & NOHZ_NEXT_KICK)
        WRITE_ONCE(nohz.needs_update, 1);
    goto abort;
}
```

The `idle_cpu()` function checks whether a CPU has any runnable tasks (by examining `rq->nr_running` and `rq->ttwu_pending`). If `idle_cpu(this_cpu)` returns true, the CPU is genuinely idle — no task has been woken up on it. In this case, the `TIF_NEED_RESCHED` flag must be from the IPI optimization (or some other non-task-wakeup source), and the ILB should continue its work. If `idle_cpu(this_cpu)` returns false, a real task wakeup has occurred, and the ILB should bail out to let the CPU schedule the new task.

This fix correctly distinguishes between two scenarios:
- **Genuine task wakeup on ILB CPU**: `idle_cpu(this_cpu)` is false (due to `rq->nr_running > 0` or `rq->ttwu_pending == 1`), AND `need_resched()` is true. The ILB should abort. Both conditions are checked.
- **IPI optimization artifact**: `idle_cpu(this_cpu)` is true (no task wakeup), but `need_resched()` is true (from `set_nr_if_polling()`). The ILB should continue. The `!idle_cpu()` check prevents the abort.

The fix is correct and complete because `idle_cpu()` accurately reflects whether a real task wakeup has occurred, even in race conditions. As documented in the Patch 2 commit message from the same series, the races between task wakeup and ILB have been carefully analyzed: (a) if `ttwu_queue_cond()` offloads the wakeup via IPI, `rq->ttwu_pending` is set before the IPI, so `idle_cpu()` sees it; (b) if the wakeup is done locally, `rq->nr_running` is incremented before `TIF_NEED_RESCHED` is set; (c) if the CPU is in `TIF_POLLING_NRFLAG` mode and the waker sets `TIF_NEED_RESCHED` via the optimization, `idle_cpu()` will still detect `ttwu_pending` or `nr_running`.

The commit message also notes that in `PREEMPT_RT` or `threadirqs` configurations, the idle load balancing may be inhibited if ksoftirqd handles the SCHED_SOFTIRQ (making the CPU appear busy). However, in these cases, when ksoftirqd finishes and goes back to sleep, it will be the only CFS task on the runqueue, triggering a newidle balance that can compensate for any missed ILB work.

## Triggering Conditions

The bug requires the following conditions to be met simultaneously:

1. **TIF_POLLING_NRFLAG idle mode**: The ILB CPU must be idle in a mode that sets `TIF_POLLING_NRFLAG`. On x86, this happens automatically when `mwait_idle_with_hints()` is used (common on Intel CPUs with C-states), or when the kernel is booted with the `idle=poll` parameter. On ARM, certain `cpuidle` drivers may also use polling.

2. **IPI optimization active**: The kernel must include commit `b2a02fc43a1f` ("smp: Optimize send_call_function_single_ipi()"), which is present in all kernels v5.8-rc1 and later. This commit causes `send_call_function_single_ipi()` to set `TIF_NEED_RESCHED` instead of sending a hardware IPI when the target CPU is polling.

3. **NOHZ idle load balancing triggered**: There must be at least one busy CPU and at least one idle CPU in NOHZ mode, so that the scheduler decides to trigger idle load balancing. The busy CPU calls `trigger_load_balance()` from the timer tick, which detects the imbalance and queues `nohz_csd_func()` on the chosen ILB CPU.

4. **Softirq processing before __schedule()**: The `SCHED_SOFTIRQ` raised by `nohz_csd_func()` must be processed in the `do_softirq_post_smp_call_flush()` path (or equivalent softirq processing), which runs after `flush_smp_call_function_queue()` but before `schedule_idle()` calls `__schedule()`. This is the normal path when the ILB is triggered via the IPI optimization.

5. **Multi-CPU system**: At least 2 CPUs are required, but the bug is more impactful with more CPUs because the ILB iterates over all NOHZ idle CPUs. With more CPUs, the premature abort affects more CPUs' load balancing.

6. **Load imbalance exists**: There must be sufficient load imbalance to warrant migration. If all CPUs are perfectly balanced, the ILB abort has no observable effect.

The bug is highly reproducible on any x86 server system running v5.8+ kernels because `mwait_idle_with_hints()` is the default idle mechanism on most Intel (and many AMD) processors, meaning `TIF_POLLING_NRFLAG` is set during idle on these systems. Every ILB kick on such systems triggers the IPI optimization path, and every resulting `_nohz_idle_balance()` invocation will see a spurious `need_resched()`. The bug is essentially guaranteed to fire on every ILB cycle on these systems, making it a systematic rather than a racy issue.

In QEMU environments, the idle mechanism depends on the emulated CPU features. With `-cpu max` or `-cpu host`, `mwait` may or may not be available. However, booting the kernel with `idle=poll` forces `cpu_idle_poll()` which always sets `TIF_POLLING_NRFLAG`, reliably enabling the bug trigger path regardless of the emulated CPU's idle capabilities.

## Reproduce Strategy (kSTEP)

This bug can be reproduced entirely through natural kernel behavior by using the `idle=poll` boot parameter to force `TIF_POLLING_NRFLAG` on all idle CPUs, creating a load imbalance to trigger the idle load balancer (ILB), and then observing (read-only) whether the ILB completes its work or aborts spuriously. No writes to internal scheduler state (such as `TIF_NEED_RESCHED`, nohz flags, or runqueue fields) are needed — the kernel's own IPI optimization path sets `TIF_NEED_RESCHED` naturally when it kicks an idle polling CPU for load balancing.

### Configuration

The driver should be configured with `num_cpus = 4` (CPU 0 reserved for the kSTEP driver, CPUs 1–3 for the test workload) and `params = ["idle=poll"]`. The `idle=poll` boot parameter forces all CPUs to use `cpu_idle_poll()` as their idle routine. This function sets `TIF_POLLING_NRFLAG` on the idle task's `thread_info`, which is the prerequisite for the IPI optimization in `send_call_function_single_ipi()`. kSTEP's default boot arguments already include `isolcpus=nohz,managed_irq,1-3` and `nohz_full=1-3`, which put CPUs 1–3 into nohz_full mode. When these CPUs go idle, they are added to `nohz.idle_cpus_mask`, enabling the ILB mechanism. Combined with `idle=poll`, this creates exactly the conditions under which the bug manifests on real hardware: idle CPUs polling with `TIF_POLLING_NRFLAG`, nohz mode active, and the IPI optimization overloading `TIF_NEED_RESCHED` for CSD delivery.

### Topology and Task Setup

In `setup()`, initialize a simple scheduling domain topology using `kstep_topo_init()` and `kstep_topo_apply()`. A single MC (machine-clear) group spanning CPUs 1–3 is sufficient — this ensures all test CPUs share a scheduling domain so the load balancer considers them for task migration. No complex NUMA or SMT topology is needed. Create three CFS tasks with `kstep_task_create()`. These tasks will serve as the workload that creates load imbalance on one CPU while leaving others idle.

### Creating Natural Load Imbalance

In `run()`, pin all three tasks to CPU 1 using `kstep_task_pin(p, 1, 2)` (affinity restricted to CPU 1 only), then wake them all with `kstep_task_wakeup(p)`. Advance 10–20 ticks with `kstep_tick_repeat(20)` to let the tasks settle on CPU 1 and establish their PELT load signals. At this point, CPU 1 has `nr_running == 3` while CPUs 2–3 are completely idle. This is verified by reading `cpu_rq(cpu)->nr_running` for each CPU (read-only). Next, widen each task's CPU affinity to span CPUs 1–3 by calling `kstep_task_pin(p, 1, 4)` for each task. This makes them eligible for migration by the load balancer — the tasks are still running on CPU 1, but their `cpus_mask` now permits CPUs 2 and 3. The scheduler will not spontaneously move them; only the load balancer (triggered via ILB or newidle balance) can pull them to idle CPUs.

### Natural ILB Trigger Path

After expanding affinity, advance ticks with `kstep_tick_repeat(200)`. On each tick on CPU 1 (which has tasks running), `trigger_load_balance()` is called. This function detects that nohz idle CPUs exist (CPUs 2–3 are in `nohz.idle_cpus_mask`) and that the system has a load imbalance. It selects one of the idle CPUs as the ILB CPU and queues `nohz_csd_func()` on it via `smp_call_function_single_async()`. Internally, `send_call_function_single_ipi()` checks whether the target CPU's idle task has `TIF_POLLING_NRFLAG` set. Because `idle=poll` is active, it does — so instead of sending a hardware IPI, the optimization atomically sets `TIF_NEED_RESCHED` on the idle task via `set_nr_if_polling()`. The idle CPU then exits its poll loop, calls `flush_smp_call_function_queue()` (which executes `nohz_csd_func()`), and raises `SCHED_SOFTIRQ`. The softirq handler invokes `_nohz_idle_balance()` — and at this point, `TIF_NEED_RESCHED` is still set because `__schedule()` has not yet run. On the **buggy kernel**, the `need_resched()` check in the ILB loop returns true, causing an immediate `goto abort` that skips all load balancing work. On the **fixed kernel**, the added `!idle_cpu(this_cpu)` guard recognizes that the CPU is still genuinely idle (no tasks woken on it), bypasses the abort, and allows the ILB to iterate over idle CPUs and perform `sched_balance_domains()` to pull tasks from overloaded CPU 1. This entire sequence happens naturally from kernel code paths with no driver intervention beyond creating the initial workload imbalance.

### Read-Only Observation via Callbacks

The driver uses two callbacks for passive observation. First, `on_sched_softirq_begin` fires when `SCHED_SOFTIRQ` begins processing on any CPU. In this callback, the driver reads (but never writes) `smp_processor_id()`, `need_resched()`, and `idle_cpu(cpu)` to record the state at ILB entry. A per-driver counter `softirq_on_idle_count` tracks how many times the softirq fires on an idle CPU with `need_resched()` true — this is the exact condition that triggers the bug. This counter serves as proof that the IPI optimization path was exercised and the bug-triggering state was reached naturally. Second, `on_sched_balance_begin(int cpu, struct sched_domain *sd)` fires each time `sched_balance_domains()` is called from within `_nohz_idle_balance()`. Each firing means the ILB successfully processed one CPU in its iteration loop without aborting. A counter `balance_iterations` tracks total firings. On the buggy kernel, the ILB aborts on the first `need_resched()` check, so `balance_iterations` will be zero or very low (at most 1 if the abort check follows the first balance). On the fixed kernel, the ILB completes its full loop, and `balance_iterations` will be substantially higher (proportional to the number of idle CPUs times the number of ILB kicks).

```c
static int softirq_on_idle_count = 0;
static int balance_iterations = 0;

void on_sched_softirq_begin(void) {
    int cpu = smp_processor_id();
    if (cpu != 0 && idle_cpu(cpu) && need_resched())
        softirq_on_idle_count++;
}

void on_sched_balance_begin(int cpu, struct sched_domain *sd) {
    if (cpu != 0)
        balance_iterations++;
}
```

### Task Placement as Primary Signal

After the 200-tick observation window, the driver checks the final CPU assignment of each task by reading `task_cpu(p)` — a read-only access to `p->cpu`. On the **fixed kernel**, the ILB successfully invokes `sched_balance_domains()` for each idle CPU, detects the imbalance (CPU 1 has 3 tasks, CPUs 2–3 have 0), and pulls tasks to the idle CPUs. After 200 ticks, at least 1–2 of the 3 tasks should have migrated to CPUs 2 or 3, resulting in a roughly balanced distribution (e.g., 1 task per CPU). On the **buggy kernel**, the ILB aborts on every invocation due to the spurious `need_resched()`, so no active load balancing occurs for the idle CPUs. The only remaining migration path is newidle balance (which fires when a CPU's runqueue becomes empty and it searches for work), but since CPUs 2–3 never had tasks to begin with, newidle balance is not triggered on them. The tasks remain stuck on CPU 1.

### Additional Read-Only Diagnostics

For richer diagnostic output, the driver can use `on_tick_begin` set to `kstep_output_nr_running` to log the per-CPU `nr_running` count on every tick — this produces a time-series showing whether load spreading occurs. The driver can also read `cpu_rq(cpu)->nr_running` directly in `run()` after the observation window to take a snapshot of the final load distribution. Reading the runqueue's `next_balance` field (via `cpu_rq(cpu)->next_balance`) can confirm whether the ILB updated balance timestamps for idle CPUs — on the buggy kernel, `next_balance` for CPUs 2–3 will be stale (never updated by ILB), while on the fixed kernel it will reflect recent balance activity. All of these are read-only accesses to scheduler internals via `cpu_rq()`, which is explicitly permitted.

### Pass/Fail Criteria

The driver reports results using `kstep_pass()` and `kstep_fail()` based on two signals. The primary signal is task distribution: count how many of the 3 tasks remain on CPU 1 after the observation window. If all 3 tasks are still on CPU 1 AND `softirq_on_idle_count > 0` (proving the ILB was triggered and the bug condition was reached), report `kstep_fail()` — the ILB was neutralized by the spurious `need_resched()`. If tasks are spread across CPUs 1–3 (at least one task migrated), report `kstep_pass()` — the ILB operated correctly. The secondary signal is `balance_iterations`: a value near zero despite many ticks and ILB kicks confirms the ILB aborted repeatedly, while a value proportional to `(number of idle CPUs) × (number of ILB kicks)` confirms successful iteration. If `softirq_on_idle_count == 0`, it means the IPI optimization path was not exercised (perhaps `idle=poll` was not effective), and the driver should report this as an inconclusive result rather than pass or fail.

### Driver Definition

```c
KSTEP_DRIVER_DEFINE{
    .name = "lb_ilb_spurious_need_resched",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .on_sched_softirq_begin = on_sched_softirq_begin,
    .on_sched_balance_begin = on_sched_balance_begin,
    .step_interval_us = 100,
};
```

The driver is invoked with: `python run.py lb_ilb_spurious_need_resched --num_cpus 4 --params idle=poll`

### Determinism Considerations

Unlike strategies that directly write `TIF_NEED_RESCHED` in a callback, this approach relies on the kernel's natural IPI optimization path. However, the bug is **systematic, not racy** — it fires on every single ILB cycle when `TIF_POLLING_NRFLAG` is set on the target CPU. With `idle=poll`, this flag is always set on idle CPUs, so every ILB kick goes through the optimization path and every resulting `_nohz_idle_balance()` invocation encounters the spurious `need_resched()`. Running for 200 ticks provides ample opportunity for multiple ILB kicks (the scheduler triggers ILB roughly once per tick when imbalance exists), making the reproduction highly reliable. The `softirq_on_idle_count` counter provides a built-in liveness check to confirm the bug path was actually reached, eliminating false negatives from environmental issues.

### No kSTEP Extensions Required

This strategy uses only existing kSTEP facilities: `kstep_task_create`, `kstep_task_pin`, `kstep_task_wakeup`, `kstep_tick_repeat`, `kstep_pass`/`kstep_fail`, `kstep_output_nr_running`, `on_sched_softirq_begin`, `on_sched_balance_begin`, and read-only access to `cpu_rq()` internals. The `idle=poll` boot parameter is passed via the existing `params` field in the `Driver` dataclass. No new APIs, kernel symbol imports, or framework modifications are needed.
