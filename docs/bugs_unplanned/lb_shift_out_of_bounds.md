# LB: Shift-out-of-bounds in load_balance() detach_tasks()

**Commit:** `39a2a6eb5c9b66ea7c8055026303b3aa681b49a5`
**Affected files:** kernel/sched/fair.c, kernel/sched/sched.h
**Fixed in:** v5.13-rc1
**Buggy since:** v5.10-rc1 (commit `5a7f55590467` "sched/fair: Relax constraint on task's load during load balance")

## Bug Description

The CFS load balancer's `detach_tasks()` function uses the sched domain's `nr_balance_failed` counter as a right-shift exponent to progressively relax the load comparison threshold when migrations repeatedly fail. Commit `5a7f55590467` introduced this mechanism in v5.10-rc1, changing the previous boolean relaxation (which used `nr_balance_failed` only to decide whether to skip the `load/2 > imbalance` check) into a graduated shift: `load >> nr_balance_failed > imbalance`. The intent was to make it progressively easier to migrate tasks as balance failures accumulate, preventing unfair CPU time distribution when there are more tasks than CPUs.

However, the `nr_balance_failed` counter was not properly bounded. While the normal flow should reset it to 0 on a successful balance or cap it around `sd->cache_nice_tries + 1` after an active balance, there exists a code path where the counter increments indefinitely. Specifically, when load_balance() fails and triggers `need_active_balance()`, but the currently running task on the busiest runqueue cannot run on the pulling CPU (due to CPU affinity restrictions), the active balance is aborted with a `goto out_one_pinned` — which increments `sd->balance_interval` but does NOT reset or cap `nr_balance_failed`. Each subsequent failed balance attempt increments the counter further.

Syzbot reported instances where `nr_balance_failed` reached values of 86 and 149. When this value is used as a shift exponent for a 64-bit `unsigned long`, shifting by 64 or more bits is undefined behavior per the C standard. UBSAN correctly flags this as "shift exponent N is too large for 64-bit type 'long unsigned int'".

## Root Cause

The root cause is an unbounded shift exponent in `detach_tasks()` at the `migrate_load` case. The specific buggy line is:

```c
if ((load >> env->sd->nr_balance_failed) > env->imbalance)
    goto next;
```

Here, `load` is an `unsigned long` (64 bits on x86_64) and `env->sd->nr_balance_failed` is an `int` that can grow to arbitrarily large values. The C standard (C11 §6.5.7) states: "If the value of the right operand is negative or is greater than or equal to the width of the promoted left operand, the behavior is undefined."

The counter `nr_balance_failed` is supposed to be bounded by the active balancing logic. In `load_balance()`, when the counter reaches `sd->cache_nice_tries + 3`, the `imbalanced_active_balance()` function returns true, triggering `need_active_balance()`. The active balance path normally either:
1. Resets `nr_balance_failed` to 0 on success, or
2. Sets it to `sd->cache_nice_tries + 1` on failure.

However, there is an escape hatch. Inside the `need_active_balance()` path in `load_balance()`, after acquiring the busiest runqueue lock, the code checks:

```c
if (!cpumask_test_cpu(this_cpu, busiest->curr->cpus_ptr)) {
    raw_spin_unlock_irqrestore(&busiest->lock, flags);
    goto out_one_pinned;
}
```

The `out_one_pinned` label increments `sd->balance_interval` but does NOT modify `nr_balance_failed`. The function then returns, and the caller (`rebalance_domains()`) eventually calls `load_balance()` again, which increments `nr_balance_failed` yet again (at the `if (idle != CPU_NEWLY_IDLE) sd->nr_balance_failed++;` line). This cycle repeats indefinitely as long as the busiest CPU's current task remains pinned away from the pulling CPU.

With `sd->cache_nice_tries` typically being 1 or 2 (set in `sd_init()` based on the sched domain level), the counter should normally stay in the low single digits. But through the affinity-pinned escape path, syzbot demonstrated it reaching 86 and 149, far exceeding the 63-bit maximum safe shift for 64-bit unsigned long.

## Consequence

The immediate consequence is undefined behavior (UB) in the C language sense. On most architectures, the shift result is architecture-dependent — on x86, only the low 6 bits of the shift count are used (i.e., `load >> 149` becomes `load >> (149 & 63)` = `load >> 21`), which happens to produce a "reasonable" result. However, this is not guaranteed behavior and other architectures may produce different results, including zero or the original value.

When the kernel is built with `CONFIG_UBSAN=y` (as syzbot configurations typically are), the UBSAN runtime detects the shift-out-of-bounds and triggers a warning. With `panic_on_warn` enabled (standard syzbot configuration), this becomes a kernel panic. The syzbot reports show the full panic trace:

```
UBSAN: shift-out-of-bounds in kernel/sched/fair.c:7712:14
shift exponent 149 is too large for 64-bit type 'long unsigned int'
...
Kernel panic - not syncing: panic_on_warn set ...
```

Even without UBSAN/panic_on_warn, the unbounded growth of `nr_balance_failed` is problematic: once it exceeds 63 (on 64-bit systems), the shift reduces `load` to 0 on x86 (via the masked shift), meaning ALL tasks pass the load check regardless of actual load. This defeats the purpose of the graduated relaxation and could cause excessive task migration (cache thrashing) or otherwise suboptimal load balancing decisions. The absurdly high `balance_interval` resulting from the repeated `out_one_pinned` path also degrades load balancing responsiveness.

## Fix Summary

The fix introduces a new macro `shr_bound()` in `kernel/sched/sched.h` that caps the shift exponent to `BITS_PER_TYPE(typeof(val)) - 1`:

```c
#define shr_bound(val, shift) \
    (val >> min_t(typeof(shift), shift, BITS_PER_TYPE(typeof(val)) - 1))
```

For a 64-bit `unsigned long`, this caps the shift at 63, ensuring the behavior is always defined. The maximum shift of 63 produces a result of either 0 or 1 (for any non-zero `load`), which effectively means "always allow migration" — the same practical effect the unbounded shift had on x86, but now in a well-defined manner.

The fix in `detach_tasks()` replaces the raw shift:
```c
// Before:
if ((load >> env->sd->nr_balance_failed) > env->imbalance)
// After:
if (shr_bound(load, env->sd->nr_balance_failed) > env->imbalance)
```

The commit author (Valentin Schneider) also ran a coccinelle script to find other similar patterns in `kernel/sched/` and confirmed the only other variable-exponent shift (`rq_clock_thermal()` using `sched_thermal_decay_shift`) was already safely capped at 10. Vincent Guittot noted in the mailing list discussion that there is value in letting `nr_balance_failed` grow beyond the active balance threshold, which is why the fix addresses the shift operation itself rather than capping the counter. The fix is minimal, correct, and forward-compatible — any future shifts using the `shr_bound()` macro will automatically be safe.

## Triggering Conditions

The bug requires the following conditions to be met simultaneously:

1. **Multiple CPUs**: At least 2 CPUs are needed — one to be the busiest (source) and one to be the pulling CPU (destination) for load balancing. The syzbot configuration used 2 CPUs (Google Compute Engine).

2. **Load imbalance**: There must be a persistent load imbalance between CPUs that causes `load_balance()` to be invoked repeatedly. This can be achieved with more runnable CFS tasks than CPUs (e.g., 3+ tasks on 2 CPUs) so one CPU is always busier.

3. **Migration failure with affinity pinning**: The critical condition is that `load_balance()` must repeatedly fail to migrate tasks AND the active balance fallback must also fail due to CPU affinity. Specifically, the currently running task on the busiest CPU must have its `cpus_ptr` mask set such that it cannot run on the pulling CPU. This can be achieved by pinning a task to a specific CPU using `sched_setaffinity()`.

4. **Repeated balance cycles**: The `nr_balance_failed` counter must grow to at least 64 (for 64-bit systems) to trigger the UB. Each failed `load_balance()` call increments it by 1 (when `idle != CPU_NEWLY_IDLE`). With a balance interval that grows (doubled on each `out_one_pinned` hit), reaching 64+ failures requires sustained imbalance over many balance periods. The syzbot reproducer used a mix of perf events, network I/O, and cgroup features running with 6 processes, which created the right conditions for persistent imbalance with pinned tasks.

5. **CONFIG_UBSAN**: To actually detect the undefined behavior (rather than just silently getting wrong results), the kernel must be built with `CONFIG_UBSAN=y` and `CONFIG_UBSAN_SHIFT=y`. With `panic_on_warn`, the UBSAN report triggers a kernel panic.

The syzbot reproducer involves `perf_event_open()`, `socket/sendto` for network I/O, and runs with cgroups enabled, 6 processes, and thread colliding. The combination creates heavy system activity with task pinning from perf events, causing persistent load balancing failures through the affinity-check escape path.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

### 1. Kernel Version Too Old

The bug was introduced in commit `5a7f55590467` which was merged in **v5.10-rc1**, and the fix commit `39a2a6eb5c9b66ea7c8055026303b3aa681b49a5` was merged in **v5.13-rc1**. The bug therefore exists only in kernels from v5.10 through v5.12.x inclusive. kSTEP supports Linux v5.15 and newer only. By the time v5.15 was released, this fix had already been present for over two kernel release cycles. Checking out the parent commit (`39a2a6eb~1`) would place us at approximately v5.12-rc2, which is well below the v5.15 minimum version that kSTEP's kernel module infrastructure requires to compile and operate correctly.

### 2. Theoretical Reproducibility If Version Were Supported

If the kernel version constraint were not an issue, this bug would theoretically be reproducible in kSTEP. The core mechanism requires:

- **Multiple CPUs**: kSTEP can configure QEMU with 2+ CPUs.
- **CFS tasks with CPU affinity**: `kstep_task_create()` + `kstep_task_pin(p, cpu, cpu)` can create pinned tasks.
- **Repeated load balance invocations**: `kstep_tick_repeat()` would trigger periodic load balancing through the scheduler softirq path.
- **Monitoring `nr_balance_failed`**: kSTEP's `KSYM_IMPORT()` and `cpu_rq()` access could read `sd->nr_balance_failed` from the sched domain hierarchy.

The reproduction strategy would be:
1. Create 3+ CFS tasks on a 2-CPU system.
2. Pin one task exclusively to CPU 1 (so it can't run on CPU 0).
3. Create load imbalance by placing extra tasks on CPU 1.
4. Run many ticks — each tick triggers `rebalance_domains()` → `load_balance()`.
5. The pulling CPU (CPU 0) finds CPU 1 as busiest, fails to detach tasks (they're pinned), tries active balance, but CPU 1's current task (pinned to CPU 1 only) fails the `cpumask_test_cpu(this_cpu, busiest->curr->cpus_ptr)` check.
6. `nr_balance_failed` increments each cycle.
7. After 64+ cycles, the shift in `detach_tasks()` becomes UB.
8. With UBSAN enabled, this would be detected; without it, one could check `nr_balance_failed > 63` directly.

### 3. What Would Be Needed

To support this bug in kSTEP, the minimum requirement would be to support building against kernel v5.10–v5.12 sources. This would require:
- Updating the kSTEP build system and `driver.h` to handle API differences between v5.10-v5.12 and v5.15+.
- Adding `#if LINUX_VERSION_CODE` guards for any API changes between these versions.
- This is a fundamental infrastructure change, not a minor extension.

### 4. Alternative Reproduction Methods

Outside kSTEP, this bug can be reproduced by:
- Running the syzbot reproducer program on a v5.10–v5.12 kernel built with `CONFIG_UBSAN=y`.
- Alternatively, creating a workload with: (a) 2 CPUs, (b) multiple CFS tasks where one is pinned to a single CPU, (c) sustained imbalance so `load_balance()` is invoked repeatedly, (d) the pinned task frequently being the current task on the busiest CPU when active balance is attempted. The syzbot dashboard link (https://syzkaller.appspot.com/bug?extid=d7581744d5fd27c9fbe1) provides the full reproducer and kernel configuration.
