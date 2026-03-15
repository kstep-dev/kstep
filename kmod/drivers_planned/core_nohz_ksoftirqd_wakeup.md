# Core: Unnecessary ksoftirqd Wakeup During NOHZ Idle Load Balance

**Commit:** `e932c4ab38f072ce5894b2851fea8bc5754bb8e5`
**Affected files:** `kernel/sched/core.c`
**Fixed in:** v6.13-rc3
**Buggy since:** `b2a02fc43a1f` ("smp: Optimize send_call_function_single_ipi()"), merged in v5.8-rc1

## Bug Description

When an idle CPU is selected as the idle load balancer (ILB) in the NO_HZ subsystem, the function `nohz_csd_func()` runs on that CPU to raise `SCHED_SOFTIRQ` so that `sched_balance_softirq()` can perform load balancing on behalf of other idle CPUs. The bug is that `nohz_csd_func()` uses `raise_softirq_irqoff(SCHED_SOFTIRQ)` instead of `__raise_softirq_irqoff(SCHED_SOFTIRQ)`. The difference is critical: `raise_softirq_irqoff()` checks `!in_interrupt()` and, if true, calls `wakeup_softirqd()` to wake the per-CPU `ksoftirqd` kernel thread. The `__raise_softirq_irqoff()` variant only marks the softirq as pending without any ksoftirqd wakeup.

The problem arises specifically when the idle CPU uses a polling-based idle mechanism (TIF_POLLING_NRFLAG is set). This happens on x86 CPUs that use MWAIT-based idle states (e.g., `mwait_idle_with_hints()` which sets TIF_POLLING_NRFLAG) or when the kernel is booted with `idle=poll`. When TIF_POLLING_NRFLAG is set on the idle CPU, the `send_call_function_single_ipi()` optimization (introduced by commit `b2a02fc43a1f`) avoids sending a real IPI. Instead, it simply sets TIF_NEED_RESCHED via `set_nr_if_polling()` to pull the CPU out of its polling loop. The CPU then processes the queued SMP call function via `flush_smp_call_function_queue()` in the idle task's context—crucially, NOT in hardirq context.

Because `flush_smp_call_function_queue()` runs in the idle task's context (not within an interrupt handler), the `in_interrupt()` check in `raise_softirq_irqoff()` returns false. Combined with `should_wake_ksoftirqd()` returning true (always true on non-PREEMPT_RT), this causes `wakeup_softirqd()` to be called, placing `ksoftirqd/N` on the CPU's runqueue. The softirq itself IS still processed correctly (via `do_softirq_post_smp_call_flush()` on the idle exit path), making the ksoftirqd wakeup entirely redundant. But the wakeup has a damaging side effect: it makes the idle CPU appear non-idle to the scheduler.

When the actual load balancing code runs (`_nohz_idle_balance()` within `sched_balance_softirq()`), it iterates over `nohz.idle_cpus_mask` and checks `idle_cpu(balance_cpu)` for each candidate. Since `ksoftirqd` is now on the ILB CPU's runqueue, `idle_cpu()` returns false for that CPU, and the load balancer skips it. If the ILB CPU is the only idle CPU in the system—a common scenario in lightly-loaded NUMA systems—no load balancing occurs at all, leaving imbalances uncorrected.

## Root Cause

The root cause is a mismatch between the execution context assumptions in `nohz_csd_func()` and the actual execution context after the IPI optimization in commit `b2a02fc43a1f`. Before that optimization, SMP call functions were always delivered via real IPIs, so `flush_smp_call_function_queue()` always ran in hardirq context, and `in_interrupt()` always returned true in `raise_softirq_irqoff()`. The ksoftirqd wakeup path was never triggered.

After commit `b2a02fc43a1f`, when the target CPU has TIF_POLLING_NRFLAG set, no IPI is sent. Instead, `set_nr_if_polling()` sets TIF_NEED_RESCHED, and the CPU exits its polling loop. The call function queue is then processed via `flush_smp_call_function_queue()` in `do_idle()`'s exit path, which runs in the idle task's process context with preempt_count reflecting no hardirq nesting. Here is the specific code path on the buggy kernel:

```c
static void nohz_csd_func(void *info)
{
    struct rq *rq = info;
    int cpu = cpu_of(rq);
    unsigned int flags;

    flags = atomic_fetch_andnot(NOHZ_KICK_MASK | NOHZ_NEWILB_KICK, nohz_flags(cpu));
    WARN_ON(!(flags & NOHZ_KICK_MASK));

    rq->idle_balance = idle_cpu(cpu);
    if (rq->idle_balance) {
        rq->nohz_idle_balance = flags;
        raise_softirq_irqoff(SCHED_SOFTIRQ);  // BUG: may wake ksoftirqd
    }
}
```

The `raise_softirq_irqoff()` function is:

```c
inline void raise_softirq_irqoff(unsigned int nr)
{
    __raise_softirq_irqoff(nr);

    if (!in_interrupt() && should_wake_ksoftirqd())
        wakeup_softirqd();
}
```

When called from the polling wakeup path, `in_interrupt()` returns 0 (preempt_count has no HARDIRQ_OFFSET set). On non-PREEMPT_RT kernels, `should_wake_ksoftirqd()` always returns true. Therefore `wakeup_softirqd()` is called, which does `wake_up_process(ksoftirqd)`, placing the ksoftirqd thread on the local CPU's runqueue.

Prateek Nayak's tracing data confirms this. When C2 (I/O port based C-state on AMD, which does NOT set TIF_POLLING_NRFLAG) is used, the hardirq flag is set (`d.h1.`) and `in_interrupt()` returns non-zero (65536 = HARDIRQ_OFFSET). When only C0/C1 (polling/MWAIT, which DO set TIF_POLLING_NRFLAG) are available, the hardirq flag is clear (`d..1.`) and `in_interrupt()` returns 0, triggering the spurious ksoftirqd wakeup.

## Consequence

The primary observable consequence is degraded idle load balancing effectiveness. When `ksoftirqd/N` is placed on the ILB CPU's runqueue, `idle_cpu(N)` returns false. The `_nohz_idle_balance()` function iterates over `nohz.idle_cpus_mask` checking `idle_cpu(balance_cpu)` for each CPU. If the ILB CPU is the only idle CPU, no CPU passes this check, and the entire idle load balancing pass is skipped.

Julia Lawall reported this exact scenario on a dual-socket system: with an overload of one thread on one socket and an underload on the other, all threads marked by NUMA balancing as preferring their current location, the single idle CPU chosen as ILB would wake its ksoftirqd, then `_nohz_idle_balance()` would find no idle CPUs (since the ILB CPU itself appears busy), and no migration would occur. She added tracing to `_nohz_idle_balance()`:

```
trace_printk("searching for a cpu\n");
for_each_cpu_wrap(balance_cpu, nohz.idle_cpus_mask, this_cpu+1) {
    if (!idle_cpu(balance_cpu))
        continue;
    trace_printk("found an idle cpu\n");
```

The "searching for a cpu" message appeared but "found an idle cpu" never did, because `ksoftirqd` on the runqueue made the only idle CPU appear busy.

The trace output from the commit message shows the sequence clearly:

```
<idle>-0   [000] dN.1.:  nohz_csd_func: Raising SCHED_SOFTIRQ from nohz_csd_func
<idle>-0   [000] dN.4.:  sched_wakeup: comm=ksoftirqd/0 pid=16 prio=120 target_cpu=000
<idle>-0   [000] .Ns1.:  softirq_entry: vec=7 [action=SCHED]
<idle>-0   [000] .Ns1.:  softirq_exit: vec=7  [action=SCHED]
<idle>-0   [000] d..2.:  sched_switch: swapper/0 ==> ksoftirqd/0
ksoftirqd/0-16  [000] d..2.:  sched_switch: ksoftirqd/0 ==> swapper/0
```

The ksoftirqd wakes, runs (finds nothing to do since the softirq was already processed), and sleeps. But the damage is done during the softirq processing: `sched_balance_softirq()` ran while ksoftirqd was on the runqueue.

This is not a crash or security issue, but a performance regression that can cause persistent load imbalances on systems using MWAIT-based idle states (common on modern Intel and AMD x86 CPUs). The impact is most severe on NUMA systems where only one idle CPU exists (e.g., lightly loaded systems with many sockets).

## Fix Summary

The fix replaces `raise_softirq_irqoff(SCHED_SOFTIRQ)` with `__raise_softirq_irqoff(SCHED_SOFTIRQ)` in `nohz_csd_func()`. The `__raise_softirq_irqoff()` function only sets the softirq pending bit (`or_softirq_pending(1UL << nr)`) without ever attempting to wake ksoftirqd. This is safe because the SMP call function is always invoked on the target CPU in a context where soft interrupts are guaranteed to be processed afterward:

- If the CPU was woken by a real IPI (hardirq context), softirqs are processed on IRQ exit via `irq_exit()` → `invoke_softirq()`.
- If the CPU was woken via the polling path (non-hardirq context), softirqs are processed via `do_softirq_post_smp_call_flush()` which is called right after `flush_smp_call_function_queue()` in the idle exit path.

In both cases, the SCHED_SOFTIRQ will be serviced without ksoftirqd involvement. The fix eliminates the unnecessary ksoftirqd wakeup, ensuring that `idle_cpu()` returns true for the ILB CPU when `_nohz_idle_balance()` runs. The trace output after the fix confirms no ksoftirqd wakeup:

```
<idle>-0   [000] dN.1.: nohz_csd_func: Raising SCHED_SOFTIRQ for nohz_idle_balance
<idle>-0   [000] dN.1.: softirq_raise: vec=7 [action=SCHED]
<idle>-0   [000] .Ns1.: softirq_entry: vec=7 [action=SCHED]
```

This patch is part 4 of a 4-patch series ("sched/fair: Idle load balancer fixes for fallouts from IPI optimization to TIF_POLLING CPUs"). The other patches address related issues: Patch 1 allows SCHED_SOFTIRQ from SMP-call-function on RT kernels, Patch 2 removes a stale `need_resched()` check in `nohz_csd_func()`, and Patch 3 fixes an `idle_cpu()` check ordering in `sched_balance_softirq()`.

## Triggering Conditions

The following conditions must ALL be met to trigger the bug:

1. **CONFIG_NO_HZ_COMMON** (or CONFIG_NO_HZ_IDLE/CONFIG_NO_HZ_FULL) must be enabled. This is standard on most kernel configurations and enables the nohz idle load balancer mechanism.

2. **TIF_POLLING_NRFLAG idle mode**: The idle CPU must use a polling-based idle mechanism that sets TIF_POLLING_NRFLAG. This occurs when:
   - The CPU uses MWAIT-based idle (common on Intel CPUs with `intel_idle` driver, and AMD CPUs with C0/C1 states).
   - The kernel is booted with `idle=poll`.
   - The CPU uses `cpu_idle_poll()` for any reason.
   Without TIF_POLLING_NRFLAG, real IPIs are sent, `nohz_csd_func()` runs in hardirq context, `in_interrupt()` returns true, and ksoftirqd is NOT woken.

3. **At least 2 CPUs** (one busy, one idle). The busy CPU's tick calls `trigger_load_balance()` → `nohz_balancer_kick()` to kick the idle CPU as ILB.

4. **Load imbalance**: There must be an imbalance that triggers the nohz balancer kick. Specifically, `nohz_balancer_kick()` must determine that idle load balancing is needed (e.g., `NOHZ_STATS_KICK` or `NOHZ_BALANCE_KICK` flags).

5. **Idle CPU in nohz.idle_cpus_mask**: The idle CPU must have entered the nohz idle state and be registered in `nohz.idle_cpus_mask`. This happens naturally when a CPU enters the idle loop and the tick is stopped.

6. **Most impactful when the ILB CPU is the ONLY idle CPU**: The bug's effect is most severe when the ILB CPU is the only CPU in `nohz.idle_cpus_mask`. In this case, `_nohz_idle_balance()` finds zero idle CPUs to balance for, and no load balancing occurs. With multiple idle CPUs, only the ILB CPU's own balance is skipped (still a bug, but less impactful).

The bug is deterministic given the above conditions—it triggers every time the nohz CSD is processed via the polling path. The probability of triggering depends on the system's idle state configuration: on systems with MWAIT or polling idle, it triggers consistently; on systems using HLT-only idle, it never triggers.

## Reproduce Strategy (kSTEP)

Reproducing this bug in kSTEP requires setting up a nohz idle load balancing scenario where the ILB CPU processes the nohz CSD via the polling path (not hardirq context). The key challenge is establishing the nohz state on the idle CPU without directly writing internal scheduler fields such as `tick_sched.tick_stopped` or `nohz.idle_cpus_mask`. This strategy uses boot parameters (`idle=poll`, `nohz_full=`) and natural kernel idle paths to achieve the required state, with only read-only access to internals for verification.

### Boot and Kernel Configuration

Configure the QEMU VM with 3 CPUs (`num_cpus = 3`): CPU 0 for the kSTEP driver, CPU 1 as the busy CPU, and CPU 2 as the sole idle ILB candidate. Pass the following kernel boot parameters via `run.py`:

- `idle=poll`: Forces all CPUs to use `cpu_idle_poll()`, which sets `TIF_POLLING_NRFLAG` on idle CPUs. This is the critical condition that causes `send_call_function_single_ipi()` to skip the real IPI and instead use `set_nr_if_polling()`. Without this, QEMU CPUs use HLT-based idle, `nohz_csd_func()` runs in hardirq context, and the bug cannot manifest.
- `nohz_full=2`: Configures CPU 2 as a nohz_full CPU. This ensures that when CPU 2 enters the idle loop, the kernel's own `tick_nohz_idle_enter()` path stops the tick and `nohz_balance_enter_idle()` registers CPU 2 in `nohz.idle_cpus_mask`. This is the proper kernel mechanism for establishing nohz state—no direct writes to internal fields are needed.

The kernel must be built with `CONFIG_NO_HZ_FULL=y` (which implies `CONFIG_NO_HZ_COMMON`). This is required for `nohz_full=` to take effect and for the nohz idle load balancer infrastructure to be compiled in.

### Setup Phase

Create 2–3 CFS tasks using `kstep_task_create()` and pin all of them to CPU 1 using `kstep_task_pin(p, 1, 2)`. Wake all tasks with `kstep_task_wakeup(p)`. This creates a deliberate overload on CPU 1 while leaving CPU 2 with no runnable tasks. CPU 2 will naturally enter the `do_idle()` loop when kSTEP yields control.

Ensure no tasks are placed on CPU 2. Having CPU 2 as the only idle CPU is important for maximal bug impact: `_nohz_idle_balance()` iterates over `nohz.idle_cpus_mask` and, on the buggy kernel, finds zero idle CPUs (since ksoftirqd on the runqueue makes the ILB CPU appear busy via `idle_cpu()` returning false), causing the entire load balancing pass to be skipped.

### Establishing NOHZ State Naturally

After the initial task setup, use `kstep_sleep()` (or a small series of `kstep_sleep()` calls) to relinquish control and allow the kernel's natural scheduling and idle paths to execute on all CPUs. During this time:

1. CPU 1 runs the pinned tasks normally with its tick active.
2. CPU 2, having no runnable tasks, enters the `do_idle()` loop. Because `nohz_full=2` was specified at boot, the idle path calls `tick_nohz_idle_enter()` → `tick_nohz_idle_stop_tick()`, which sets `ts->tick_stopped = 1` within the kernel's own code. The function `nohz_balance_enter_idle()` is then called by the idle path, which adds CPU 2 to `nohz.idle_cpus_mask` and sets the per-CPU `nohz_tick_stopped` flag.
3. Because `idle=poll` is active, CPU 2 enters `cpu_idle_poll()`, which sets `TIF_POLLING_NRFLAG` on its idle thread. This is the flag that `send_call_function_single_ipi()` checks to decide whether to send a real IPI or just set `TIF_NEED_RESCHED`.

Even if kSTEP's initialization temporarily disturbs the `tick_sched` state (e.g., via `kstep_disable_sched_timer()` zeroing the structure), the kernel will re-establish it when CPU 2 naturally re-enters the idle loop. The `nohz_full=2` boot parameter ensures the kernel intends to stop the tick on CPU 2, and the `do_idle()` → `tick_nohz_idle_enter()` code path will restore it. Using `kstep_sleep()` gives the kernel sufficient time for this natural re-initialization to complete.

To verify (read-only) that the nohz state is correctly established before proceeding, import the nohz structure via `KSYM_IMPORT(nohz)` and read `nohz.idle_cpus_mask` to confirm CPU 2's bit is set. Also read `cpu_rq(2)->nr_running` to confirm it is 0. These are purely observational checks with no writes. If CPU 2 is not yet in the nohz mask, call `kstep_sleep()` again to give the kernel more time, or use `kstep_sleep_until(fn)` with a condition function that checks `cpumask_test_cpu(2, nohz.idle_cpus_mask)`.

### Triggering the NOHZ ILB Kick

Once the nohz state is confirmed via read-only checks, use `kstep_sleep()` to let natural ticks continue on CPU 1. On each tick, CPU 1 calls `sched_tick()` → `trigger_load_balance()` → `nohz_balancer_kick()`. Because CPU 1 is overloaded (multiple tasks pinned) and CPU 2 is registered in `nohz.idle_cpus_mask`, the nohz balancer kick will queue the `nohz_csd` to CPU 2 with `NOHZ_BALANCE_KICK` or `NOHZ_STATS_KICK` flags. Alternatively, `kstep_tick_repeat(n)` can be used if more deterministic tick delivery is desired, but `kstep_sleep()` is preferred here because it avoids any interference with the natural `flush_smp_call_function_queue()` context on CPU 2.

When the nohz CSD reaches CPU 2, because `TIF_POLLING_NRFLAG` is set (from `idle=poll`), no real IPI is sent. Instead, `set_nr_if_polling()` sets `TIF_NEED_RESCHED` on CPU 2's idle thread, pulling it out of the poll loop. CPU 2 then calls `flush_smp_call_function_queue()` in the idle task's context (NOT hardirq context). The queued `nohz_csd_func()` executes in this non-interrupt context.

On the **buggy kernel**: `nohz_csd_func()` calls `raise_softirq_irqoff(SCHED_SOFTIRQ)`. Since `in_interrupt()` returns 0 (no `HARDIRQ_OFFSET` in `preempt_count`) and `should_wake_ksoftirqd()` returns true (always true on non-PREEMPT_RT), `wakeup_softirqd()` wakes `ksoftirqd/2`, placing it on CPU 2's runqueue. When `sched_balance_softirq()` subsequently runs and iterates over `nohz.idle_cpus_mask`, `idle_cpu(2)` returns false (because `ksoftirqd/2` is on the runqueue with `nr_running >= 1`), and CPU 2 is skipped for load balancing.

On the **fixed kernel**: `nohz_csd_func()` calls `__raise_softirq_irqoff(SCHED_SOFTIRQ)`, which only marks the softirq as pending without waking ksoftirqd. When `sched_balance_softirq()` runs, `idle_cpu(2)` returns true, and load balancing proceeds normally.

### Detection via on_sched_softirq_begin Callback

Register an `on_sched_softirq_begin` callback in the driver. When this callback fires on CPU 2 (check `smp_processor_id() == 2`), perform read-only observations:

1. Read `cpu_rq(2)->nohz_idle_balance` to confirm this is the ILB CPU processing a nohz balance request (non-zero means ILB flags are set). If zero, this softirq is not an ILB event—skip it.
2. Read `cpu_rq(2)->nr_running`. **Buggy kernel**: `nr_running >= 1` (ksoftirqd is on the runqueue). **Fixed kernel**: `nr_running == 0`.
3. Import and call `idle_cpu` via `KSYM_IMPORT(idle_cpu)` and check `idle_cpu(2)`. **Buggy kernel**: returns 0 (not idle). **Fixed kernel**: returns 1 (idle).

If `nr_running > 0` and `idle_cpu(2) == 0` during the ILB softirq, this confirms the spurious ksoftirqd wakeup (buggy behavior). Call `kstep_fail("ksoftirqd spuriously woken: nr_running=%d idle_cpu=%d", nr_running, idle_val)`. If `nr_running == 0` and `idle_cpu(2) == 1`, the fix is working correctly. Call `kstep_pass("no spurious ksoftirqd wakeup: ILB CPU remains idle")`.

### Alternative Detection: Migration-Based Observation

As a complementary detection method that requires no internal state access at all, observe the externally visible consequence of the bug—failed task migration:

1. After pinning 2–3 tasks to CPU 1 and allowing sufficient time for the idle load balancer to act (e.g., multiple `kstep_sleep()` calls), check which CPU each task is running on by reading `task->cpu` or using `task_cpu()`.
2. **Buggy kernel**: All tasks remain on CPU 1 because the ILB pass was skipped (CPU 2 appeared non-idle due to ksoftirqd).
3. **Fixed kernel**: At least one task migrated to CPU 2 because the ILB correctly identified CPU 2 as idle and performed load balancing.

This approach detects the consequence of the bug rather than the internal mechanism. It is less precise—migration depends on load balancer heuristics, task weights, cache affinity, and timing—but it is a useful secondary check alongside the callback-based detection.

### Key Internal Symbols (Read-Only)

The following symbols are accessed for observation and verification only—no writes are performed:

- `cpu_rq(cpu)` — read `nr_running`, `nohz_idle_balance`, and `idle_balance` fields to verify ILB state and runqueue occupancy.
- `KSYM_IMPORT(idle_cpu)` — call to check if CPU appears idle. This is a kernel function call, not a direct write to any structure.
- `KSYM_IMPORT(nohz)` — read `nohz.idle_cpus_mask` to verify that the nohz state was naturally established by the kernel's idle path.
- `smp_processor_id()` — identify which CPU is executing the callback.

No writes to `tick_sched`, `nohz_flags`, `nohz.idle_cpus_mask`, or any other internal scheduler structure are required. The `idle=poll` and `nohz_full=2` boot parameters, combined with the kernel's natural idle path, establish all required state through the kernel's own code paths.

### Potential Complications

- **Timing sensitivity**: The nohz state on CPU 2 must be established before the ILB kick from CPU 1. Using `kstep_sleep_until(fn)` with a read-only check on `nohz.idle_cpus_mask` ensures proper ordering without any writes.
- **kSTEP tick interference**: If kSTEP's tick mechanism dispatches SMP calls to all CPUs (including CPU 2), the flush on CPU 2 might run in a different context than the natural idle flush. Prefer `kstep_sleep()` over `kstep_tick()` for the trigger phase to let the kernel's natural tick and CSD delivery run unperturbed.
- **ILB CPU selection**: If CPU 0 (the driver CPU) also appears in `nohz.idle_cpus_mask`, the ILB might select CPU 0 instead of CPU 2. CPU 0 is running the driver and should not be truly idle, but verify by checking which CPU the `on_sched_softirq_begin` callback fires on. If needed, keep CPU 0 busy (the driver itself usually suffices).
- **ksoftirqd false positives**: If `ksoftirqd/2` was woken for another softirq reason before the ILB kick, the detection would see a false positive. Keep the test scenario minimal (no extraneous softirq sources, no network or block I/O on CPU 2) to avoid this.
- **Expected timeline**: The nohz balancer kick should occur within a few seconds of `kstep_sleep()` after the imbalance is established on CPU 1. The `on_sched_softirq_begin` callback should fire on CPU 2 once the nohz CSD is processed and `SCHED_SOFTIRQ` runs.
