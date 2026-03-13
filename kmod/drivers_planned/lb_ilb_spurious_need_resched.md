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

This bug can be reproduced with kSTEP using a multi-pronged approach. The core challenge is ensuring that the ILB CPU has `TIF_NEED_RESCHED` set (from the IPI optimization) when `_nohz_idle_balance()` runs. There are two viable strategies, and the recommended approach combines elements of both for reliability.

### Strategy A: Natural Reproduction via `idle=poll`

The most natural approach relies on enabling the `idle=poll` kernel boot parameter, which forces all idle CPUs to use `cpu_idle_poll()`. This function sets `TIF_POLLING_NRFLAG`, enabling the IPI optimization path. kSTEP would need a minor extension to support adding kernel-level boot parameters (before the `--` separator in `run.py`). The change to `run.py` would be small: add an optional `kernel_params` field to the `Driver` dataclass and prepend them to `boot_args` before the `--` separator.

With `idle=poll` active:

1. **Topology**: Configure at least 4 CPUs (CPU 0 for the driver, CPUs 1-3 for the test). Use `kstep_topo_init()` and `kstep_topo_apply()` to set up a simple scheduling domain (one MC group).

2. **Create load imbalance**: Create 2-3 CFS tasks with `kstep_task_create()` and pin them all to CPU 1 using `kstep_task_pin(p, 1, 2)`. This creates a heavily loaded CPU 1 and idle CPUs 2-3.

3. **Trigger ILB naturally**: The scheduler's `trigger_load_balance()` function, called on every timer tick, will detect the imbalance and attempt to kick an idle CPU for load balancing. With NOHZ, it queues `nohz_csd_func()` on the chosen ILB CPU. Since the idle CPUs are in `TIF_POLLING_NRFLAG` mode (from `idle=poll`), the IPI optimization sets `TIF_NEED_RESCHED`.

4. **Advance ticks**: Use `kstep_tick_repeat(n)` to advance several ticks, allowing the timer-based ILB trigger and the load balancing softirq to execute.

5. **Observe**: After sufficient ticks, check which CPUs the tasks are running on using `task_cpu(p)` or `kstep_output_curr_task()`. On the **buggy kernel**, the ILB aborts immediately due to the spurious `need_resched()`, and tasks remain pinned to CPU 1 (or take much longer to migrate). On the **fixed kernel**, the ILB completes normally, and tasks are migrated to CPUs 2-3.

### Strategy B: Direct State Simulation via `on_sched_softirq_begin` Callback

For a more deterministic approach that does not depend on `idle=poll` or QEMU's idle implementation, the driver can use the `on_sched_softirq_begin` callback to simulate the IPI optimization's effect by setting `TIF_NEED_RESCHED` on the current task when the CPU is idle. This precisely replicates the condition that exists in the real-world bug path.

1. **Import required symbols**: Use `KSYM_IMPORT` to access `set_tsk_need_resched` (or directly write to `current->thread_info.flags` via `set_tsk_thread_flag(current, TIF_NEED_RESCHED)`).

2. **Callback implementation**:
   ```c
   static int ilb_softirq_count = 0;
   
   void on_sched_softirq_begin(void) {
       int cpu = smp_processor_id();
       if (cpu != 0 && idle_cpu(cpu)) {
           /* Simulate the IPI optimization artifact:
            * Set TIF_NEED_RESCHED as if set_nr_if_polling() was called */
           set_tsk_need_resched(current);
           ilb_softirq_count++;
       }
   }
   ```

3. **Setup**: Create 4 CPUs. Pin 2-3 tasks to CPU 1, leave CPUs 2-3 idle. Use `kstep_tick_repeat()` to advance time.

4. **Detection**: After a fixed number of ticks (e.g., 50-100), check whether any tasks were migrated from CPU 1 to CPUs 2-3. On the **buggy kernel**, `_nohz_idle_balance()` will see `need_resched()` returning true and abort, so tasks remain on CPU 1. On the **fixed kernel**, the `!idle_cpu(this_cpu)` check will recognize the CPU is still idle, skip the abort, and proceed with load balancing, migrating tasks.

5. **Pass/fail criteria**:
   - If `ilb_softirq_count > 0` (confirming the callback fired) AND tasks remain on CPU 1: **fail** on buggy kernel (bug triggered).
   - If `ilb_softirq_count > 0` AND tasks are spread across CPUs 1-3: **pass** on fixed kernel.
   - Use `kstep_pass()` / `kstep_fail()` to report.

### Strategy C (Recommended): Hybrid with Direct Observation

Combine Strategy B's callback with direct observation of `_nohz_idle_balance()` behavior using additional imported symbols:

1. **Import `nohz` structure**: Use `KSYM_IMPORT_TYPED` to access the `struct nohz` global (or its relevant fields like `nohz.has_blocked` and `nohz.needs_update`). These are set to 1 when the ILB aborts early.

2. **Use `on_sched_balance_begin` callback**: This fires when `sched_balance_domains()` is called from within `_nohz_idle_balance()`. If the ILB aborts, this callback will NOT fire for the remaining CPUs. Count how many times this callback fires after creating the imbalance — fewer firings means the ILB aborted.

3. **Driver flow**:
   ```
   setup():
     - kstep_topo_init() with 4 CPUs
     - kstep_topo_apply()
   
   run():
     - Create 3 tasks, pin to CPU 1
     - kstep_tick_repeat(10)   // let tasks settle
     - Reset counters
     - kstep_tick_repeat(100)  // allow ILB to fire
     - Check: how many balance callbacks fired?
     - Check: are tasks still all on CPU 1?
     - Report pass/fail
   ```

4. **Expected results**:
   - **Buggy kernel**: `on_sched_softirq_begin` fires (ILB triggered), `on_sched_balance_begin` fires 0-1 times (ILB aborts before iterating), tasks stay on CPU 1.
   - **Fixed kernel**: Both callbacks fire, `on_sched_balance_begin` fires multiple times (once per balanced CPU), tasks distributed.

### kSTEP Extensions Needed

The following minor extensions would strengthen this reproduction:

1. **Kernel boot parameter support**: Add a `kernel_params` field to the `Driver` dataclass in `run.py` to allow drivers to specify kernel-level boot parameters (prepended before `--`). This enables `idle=poll` for Strategy A.

2. **`set_tsk_need_resched()` access**: This is already available through `<linux/sched.h>` included in `driver.h`, or can be accessed via `set_tsk_thread_flag(current, TIF_NEED_RESCHED)`. No new KSYM_IMPORT is needed for this.

3. **No other changes required**: The existing `on_sched_softirq_begin`, `on_sched_balance_begin`, `kstep_task_create`, `kstep_task_pin`, `kstep_tick_repeat`, and `KSYM_IMPORT` facilities are sufficient.

### Determinism Considerations

Strategy B is more deterministic than Strategy A because it directly controls when `TIF_NEED_RESCHED` is set. Strategy A depends on the natural ILB trigger timing, which may vary between runs. However, even with Strategy A, the bug is systematic (fires on every ILB cycle when `TIF_POLLING_NRFLAG` is set), so it should be highly reproducible after a sufficient number of ticks. Running for 100+ ticks should ensure multiple ILB attempts.

For maximum determinism, Strategy B with direct observation (Strategy C) is recommended. The callback fires deterministically when `SCHED_SOFTIRQ` is processed, and the bug manifests on every invocation of `_nohz_idle_balance()` when `need_resched()` is true.
