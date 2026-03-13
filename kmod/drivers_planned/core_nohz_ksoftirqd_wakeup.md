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

Reproducing this bug in kSTEP requires several setup steps and one minor framework extension. The core idea is to create a load imbalance, allow the nohz idle load balancer to be triggered, and detect whether ksoftirqd is spuriously woken on the ILB CPU.

### kSTEP Extensions Required

1. **Add `idle=poll` kernel boot parameter**: kSTEP must pass `idle=poll` as a kernel command line argument (BEFORE the `--` separator in `run.py`'s `boot_args`). This forces all CPUs to use `cpu_idle_poll()` which sets TIF_POLLING_NRFLAG, ensuring that `send_call_function_single_ipi()` uses the polling path instead of real IPIs. Without this, QEMU CPUs use HLT-based idle and the bug cannot manifest. Implementation: add a `kernel_params` field to the `Driver` dataclass in `run.py`, or simply add `"idle=poll"` to the `boot_args` list.

2. **Fix nohz state initialization**: kSTEP's `kstep_disable_sched_timer()` zeroes the `tick_sched` structure via `memset(ts, 0, sizeof(struct tick_sched))`. This sets `ts->tick_stopped = 0`, which causes `nohz_balance_enter_idle()` to bail out early (it checks `sched_tick_stopped(cpu)`). After zeroing, set `ts->tick_stopped = 1` (or `ts->inidle = 1` as appropriate) so idle CPUs are properly added to `nohz.idle_cpus_mask`. Alternatively, use `KSYM_IMPORT` to access `nohz.idle_cpus_mask` and manually set the bits for idle CPUs.

### Driver Design

**CPU configuration**: 3 CPUs (CPU 0 for driver, CPU 1 for overloaded tasks, CPU 2 as the sole idle ILB CPU). Set `num_cpus = 3` in the driver configuration.

**Setup phase**:
1. Create 2-3 CFS tasks using `kstep_task_create()`.
2. Pin all tasks to CPU 1 using `kstep_task_pin(p, 1, 2)` to create an overload.
3. Wake all tasks using `kstep_task_wakeup(p)`.
4. Ensure CPU 2 has no tasks (it will naturally enter the idle loop).

**Tick phase**:
1. Call `kstep_tick()` repeatedly (e.g., `kstep_tick_repeat(20)`).
2. During ticks, kSTEP's `kstep_do_sched_tick()` calls `sched_tick_fn()` on each CPU via `smp_call_function_single()`.
3. On CPU 1 (busy), `trigger_load_balance()` detects the imbalance and `nohz_balancer_kick()` queues a nohz CSD to CPU 2 (the idle CPU).
4. When kSTEP ticks CPU 2, the `smp_call_function_single()` call wakes CPU 2 from its polling idle. Since CPU 2 has TIF_POLLING_NRFLAG set (from `idle=poll`), no real IPI is sent; instead, TIF_NEED_RESCHED is set.
5. CPU 2 exits the poll loop. `flush_smp_call_function_queue()` processes the pending nohz CSD (`nohz_csd_func()`) in idle task context (non-hardirq).
6. On the **buggy kernel**: `raise_softirq_irqoff()` → `!in_interrupt()` is true → `wakeup_softirqd()` places ksoftirqd/2 on CPU 2's runqueue.
7. On the **fixed kernel**: `__raise_softirq_irqoff()` only marks SCHED_SOFTIRQ pending, no ksoftirqd wakeup.

**Detection phase** (in `on_sched_softirq_begin` callback):
1. Check `smp_processor_id()` to identify which CPU is running the softirq.
2. If this CPU is an ILB CPU (check `cpu_rq(smp_processor_id())->nohz_idle_balance != 0`):
   - Read `idle_cpu(smp_processor_id())` and `cpu_rq(smp_processor_id())->nr_running`.
   - **Buggy kernel**: `idle_cpu()` returns 0 (false) because ksoftirqd is on the runqueue; `nr_running >= 1`.
   - **Fixed kernel**: `idle_cpu()` returns 1 (true); `nr_running == 0`.
3. Use `kstep_pass()` or `kstep_fail()` to report the result.

**Alternative detection** (migration-based):
Instead of checking internal state, observe the outcome:
1. After ticking for many cycles, check which CPU each task is running on.
2. **Buggy kernel**: All tasks remain on CPU 1 (ILB failed to balance, so no migration to CPU 2).
3. **Fixed kernel**: At least one task migrated to CPU 2 (ILB successfully balanced).
4. This is less precise (depends on whether the ILB actually pulls a task) but is externally observable.

**Key internal symbols to import**:
- `KSYM_IMPORT(idle_cpu)` — to check if a CPU appears idle.
- Access `cpu_rq(cpu)->nohz_idle_balance` — to check if ILB is active (via `internal.h`).
- Access `cpu_rq(cpu)->nr_running` — to check runqueue occupancy.

**Expected timeline**: The nohz balancer kick should occur within 10-20 ticks after the imbalance is established. The `on_sched_softirq_begin` callback should fire on the ILB CPU once the nohz CSD is processed and SCHED_SOFTIRQ runs.

**Potential complications**:
- If `nohz.idle_cpus_mask` is not properly populated (due to kSTEP's tick timer disabling), the ILB will never be kicked. Verify this mask is set correctly or manually initialize it.
- The `smp_call_function_single()` ordering matters: the nohz CSD must be queued BEFORE kSTEP's tick SMP call to CPU 2, so that both are processed in the same `flush_smp_call_function_queue()` call. Since kSTEP ticks CPUs in order (CPU 1 first, then CPU 2), the nohz CSD from CPU 1's tick should be queued before CPU 2's tick call.
- kSTEP's tick handler drains SCHED_SOFTIRQ on each CPU after calling `sched_tick_fn()`. If the nohz CSD has already set SCHED_SOFTIRQ pending via `nohz_csd_func()`, kSTEP's drain will process it. The ksoftirqd wakeup happens inside `nohz_csd_func()` (before kSTEP's drain), so it's already on the runqueue when the softirq handler runs.
