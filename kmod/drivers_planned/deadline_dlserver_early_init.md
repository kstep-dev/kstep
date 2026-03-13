# Deadline: dl_server Initialization Before SMP Causes Wrong extra_bw

**Commit:** `9f239df55546ee1d28f0976130136ffd1cad0fd7`
**Affected files:** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/sched.h
**Fixed in:** v6.17-rc1
**Buggy since:** v6.12-rc1 (introduced by `d741f297bceaf` "sched/fair: Fair server interface")

## Bug Description

The SCHED_DEADLINE fair server (`rq->fair_server`) on each CPU's runqueue provides a mechanism for CFS (fair) tasks to receive guaranteed bandwidth via a DEADLINE server entity. Each per-CPU fair server is a `sched_dl_entity` configured with a default runtime of 50ms and a period of 1000ms (5% bandwidth). The fair server's bandwidth is accounted in the DEADLINE bandwidth tracking system, which distributes bandwidth adjustments across all active CPUs in a root domain.

In the buggy code, the fair server was initialized lazily inside `dl_server_start()`. The first time `dl_server_start()` was called for a given CPU's `rq->fair_server`, it would check `if (!dl_server(dl_se))` and, if the server was not yet initialized, call `dl_server_apply_params(dl_se, runtime, period, 1)` followed by setting `dl_se->dl_server = 1` and `dl_se->dl_defer = 1`. The problem is that `dl_server_start()` is called during early boot — triggered by CFS task enqueue paths — before SMP initialization completes and before all CPUs are online.

The `dl_server_apply_params()` function calls `dl_bw_cpus(cpu)`, which returns `cpumask_weight_and(rd->span, cpu_active_mask)` — the number of active CPUs in the root domain. During early boot, only the boot CPU (CPU 0) is online, so `dl_bw_cpus()` returns 1 regardless of the actual number of CPUs in the system. This incorrect CPU count propagates into the DEADLINE bandwidth accounting via `__dl_add()`, which divides the server's bandwidth by the CPU count before distributing it to each runqueue's `extra_bw`. The result is that `extra_bw` is reduced by the full server bandwidth instead of by `server_bw / N`, where N is the actual number of CPUs.

This bug was reported by Marcel Ziswiler at Codethink, who observed massive SCHED_DEADLINE deadline misses (43 million on Intel NUC, 600 thousand on RADXA ROCK5B over a week of testing) when the GRUB reclamation algorithm (`SCHED_FLAG_RECLAIM`) was enabled alongside overrunning jobs. The incorrect `extra_bw` values distort the GRUB bandwidth reclamation formula, causing tasks to consume their runtime budgets faster than expected, leading to premature throttling and deadline misses.

## Root Cause

The root cause is a boot ordering issue in the initialization of per-CPU DEADLINE fair servers. Prior to the fix, the initialization path was:

1. `dl_server_start()` is called when a CFS task is first enqueued on a runqueue (via `enqueue_task_fair()` → `dl_server_start(&rq->fair_server)`).
2. Inside `dl_server_start()`, the code checks `if (!dl_server(dl_se))` — if the server has not been initialized yet.
3. If uninitialized, it calls `dl_server_apply_params(dl_se, 50ms, 1000ms, 1)`.
4. `dl_server_apply_params()` with `init=1` calls:
   - `cpus = dl_bw_cpus(cpu)` → returns `cpumask_weight_and(rd->span, cpu_active_mask)`.
   - `__dl_add(dl_b, new_bw, cpus)` → calls `__dl_update(dl_b, -((s32)tsk_bw / cpus))`.
5. `__dl_update()` iterates over all active CPUs in the root domain and adjusts each one's `rq->dl.extra_bw` by `-tsk_bw / cpus`.

During early boot, only CPU 0 is online. The `cpu_active_mask` has only bit 0 set. So `dl_bw_cpus()` returns 1. The `__dl_add()` call computes `tsk_bw / 1 = tsk_bw` and subtracts this full amount from `extra_bw` for each active CPU (just CPU 0 at this point). When subsequent CPUs come online, their `extra_bw` starts from `max_bw` (set in `init_dl_rq_bw_ratio()`), but CPU 0's `extra_bw` has been reduced by the full server bandwidth instead of by `server_bw / N`.

The bandwidth of the fair server is `to_ratio(1000ms, 50ms)` = approximately 52428 in the kernel's bandwidth unit representation (BW_SHIFT = 20). On a correctly initialized 4-CPU system, each CPU's `extra_bw` should be reduced by `52428 / 4 = 13107`. But on the buggy kernel, CPU 0's `extra_bw` is reduced by `52428 / 1 = 52428` — four times too much. CPUs 1–3 may also have incorrect values depending on exactly when their fair servers get initialized relative to other CPUs coming online.

Additionally, the old `dl_server_start()` code had a XXX comment acknowledging this issue: "the apply do not work fine at the init phase for the fair server because things are not yet set." The secondary issue is that `setup_new_dl_entity()` was called without first calling `update_rq_clock(rq)`, meaning `rq_clock(rq)` could be stale when setting the server's initial deadline, potentially triggering the `WARN_ON(dl_time_before(rq_clock(rq), dl_se->deadline))` check.

## Consequence

The primary consequence is incorrect GRUB (Greedy Reclamation of Unused Bandwidth) accounting for SCHED_DEADLINE tasks that use `SCHED_FLAG_RECLAIM`. The GRUB algorithm uses `extra_bw` to compute the active utilization (`u_act`) in the `grub_reclaim()` function:

```c
u_act = rq->dl.max_bw - u_inact - rq->dl.extra_bw;
```

When `extra_bw` is too low (because too much was subtracted), `u_act` becomes artificially large. This larger `u_act` value is used to scale execution time: `scaled_delta_exec = (delta * u_act) >> BW_SHIFT`. A larger `u_act` means the task's runtime is consumed faster in accounting terms, leading to premature budget exhaustion and throttling before the task has actually used its fair share of CPU time. The task then misses its deadline because it is throttled while waiting for budget replenishment.

In real-world testing by Marcel Ziswiler, this caused dramatic deadline misses: 43 million deadline misses on an Intel NUC (amd64) and 600 thousand on a RADXA ROCK5B (aarch64, with double the CPU cores) over the course of a week. Without `SCHED_FLAG_RECLAIM` enabled, the same test configuration with identical overrunning jobs produced zero deadline misses. The severity scales inversely with the number of CPUs — on a system with more CPUs, the overcounting of the bandwidth subtraction from `extra_bw` is proportionally larger (e.g., on 8 CPUs, the subtraction is 8x too large).

There are also secondary consequences: the `dl_server_stop()` function in the buggy code checked `if (!dl_se->dl_runtime)` instead of `if (!dl_server(dl_se) || !dl_server_active(dl_se))`, which could cause it to attempt to dequeue a server that was never properly enqueued, or fail to stop a server that should be stopped. These conditions could lead to additional inconsistencies in the DEADLINE runqueue state.

## Fix Summary

The fix moves the fair server initialization out of the lazy `dl_server_start()` path and into a new dedicated function `sched_init_dl_servers()`, which is called from `sched_init_smp()` after SMP initialization is complete and all CPUs are online. This ensures that `dl_bw_cpus()` returns the correct total CPU count when computing bandwidth distribution.

The new `sched_init_dl_servers()` function iterates over all online CPUs using `for_each_online_cpu(cpu)`, acquires the per-runqueue lock (`guard(rq_lock_irq)(rq)`), and for each CPU: (1) calls `dl_server_apply_params(dl_se, 50ms, 1000ms, 1)` to set the bandwidth parameters with correct CPU count, (2) sets `dl_se->dl_server = 1` and `dl_se->dl_defer = 1`, and (3) calls `setup_new_dl_entity(dl_se)` to set the initial deadline. The `setup_new_dl_entity()` function is also patched to call `update_rq_clock(rq)` at the beginning, ensuring `rq_clock(rq)` is fresh when computing the initial deadline.

The `dl_server_start()` function is simplified: the lazy initialization block is removed entirely, and the guard condition changes from `if (!dl_se->dl_runtime || dl_se->dl_server_active)` to `if (!dl_server(dl_se) || dl_se->dl_server_active)`, which properly checks whether the server has been initialized (via the `dl_server` flag) rather than checking for non-zero runtime. Similarly, `dl_server_stop()` is changed from `if (!dl_se->dl_runtime)` to `if (!dl_server(dl_se) || !dl_server_active(dl_se))`, ensuring it only operates on properly initialized and active servers. The `dl_server_apply_params()` function also adds a `guard(rcu)()` call to satisfy the RCU lock requirement of `dl_bw_cpus()`.

## Triggering Conditions

The bug is triggered automatically on any multi-CPU system running a kernel between v6.12-rc1 and v6.17-rc1 (inclusive). The following conditions are necessary:

- **Multiple CPUs:** The system must have more than 1 CPU. On a single-CPU system, `dl_bw_cpus()` returns 1 both at boot and at runtime, so the accounting is accidentally correct. The more CPUs, the larger the discrepancy: on N CPUs, `extra_bw` is reduced by N times too much.

- **SCHED_DEADLINE tasks with SCHED_FLAG_RECLAIM:** The observable impact (deadline misses) requires SCHED_DEADLINE tasks using the GRUB reclamation algorithm. Without GRUB, the `extra_bw` field is not used in the scheduling formula, so the wrong values do not affect task execution.

- **No specific workload timing required:** This is not a race condition. The bug is deterministic: every boot on a multi-CPU system produces incorrect `extra_bw` values. The incorrect state persists for the entire lifetime of the kernel.

- **No special kernel config needed:** The fair server is enabled by default when `CONFIG_SMP` is set (which it is on virtually all multi-CPU kernels). `CONFIG_SCHED_DEADLINE` must be enabled (default on most configurations). No specific debugfs or sysctl settings are needed to trigger the bug — it happens automatically.

- **Observable without SCHED_DEADLINE tasks:** Even without creating SCHED_DEADLINE tasks, the incorrect `extra_bw` values can be observed by reading internal scheduler state. On a 4-CPU system, `cpu_rq(0)->dl.extra_bw` will be significantly lower than `cpu_rq(0)->dl.max_bw` minus the expected per-CPU fair server bandwidth fraction.

## Reproduce Strategy (kSTEP)

The bug is a boot-time initialization error whose effects persist throughout the kernel's lifetime. By the time a kSTEP driver module loads, the damage is already done: `extra_bw` on each CPU's `dl_rq` already contains the wrong value. The kSTEP driver therefore does not need to trigger the bug — it only needs to observe the already-incorrect state.

### Step 1: Configure QEMU with Multiple CPUs

Configure the QEMU instance with at least 4 CPUs (e.g., `NR_CPUS=4`). This ensures `dl_bw_cpus()` should return 4 at runtime but returned 1 during the buggy early-boot initialization. The larger the CPU count, the more dramatic the discrepancy.

### Step 2: Read the fair server bandwidth parameters

In the driver's `run()` function, read the fair server's configured bandwidth from `cpu_rq(cpu)->fair_server.dl_bw` for any CPU. This should be `to_ratio(1000ms, 50ms)`, approximately 52428800 (in the kernel's bandwidth unit with BW_SHIFT = 20). Use `KSYM_IMPORT` if needed to access `to_ratio` or compute it manually.

### Step 3: Read extra_bw from each CPU

For each online CPU (1 through N-1, skipping CPU 0 which runs the driver), read:
- `cpu_rq(cpu)->dl.extra_bw` — the current extra bandwidth
- `cpu_rq(cpu)->dl.max_bw` — the maximum bandwidth

### Step 4: Compute expected extra_bw

The expected `extra_bw` after fair server initialization is:
```
expected_extra_bw = max_bw - (fair_server_bw / nr_online_cpus)
```

On the buggy kernel, the actual `extra_bw` will be:
```
buggy_extra_bw ≈ max_bw - fair_server_bw  (for CPU 0, initialized with cpus=1)
```
or some intermediate wrong value for other CPUs depending on when they came online.

### Step 5: Compare and report

Compare the actual `extra_bw` with the expected value. On the buggy kernel:
- `extra_bw` on at least some CPUs will be significantly lower than expected (by a factor related to the CPU count).
- Specifically, `max_bw - extra_bw` should equal `fair_server_bw / N` on the fixed kernel, but will equal `fair_server_bw` (or `fair_server_bw / K` where K < N) on the buggy kernel.

Call `kstep_pass()` if the values match expected (fixed kernel) or `kstep_fail()` if they show the boot-time miscalculation (buggy kernel).

### Step 6: Task setup

Create at least one CFS task per CPU using `kstep_task_create()` and `kstep_task_pin()` to ensure the fair server is active on each CPU. Run a few ticks with `kstep_tick_repeat()` to let the scheduler settle, then read the `extra_bw` values.

### Step 7: Kernel version guard

Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,12,0)` since the fair server interface was introduced in v6.12-rc1 by commit `d741f297bceaf`.

### Step 8: Detection criteria

On the **buggy kernel** (v6.12-rc1 through v6.16):
- `cpu_rq(cpu)->dl.extra_bw` will show values that indicate the full fair server bandwidth was subtracted per-CPU rather than the per-CPU fraction.
- `kstep_fail("extra_bw on cpu %d is %llu, expected %llu", cpu, actual, expected)`.

On the **fixed kernel** (v6.17-rc1+):
- `cpu_rq(cpu)->dl.extra_bw` will show correctly computed values.
- `kstep_pass("extra_bw correctly initialized on all CPUs")`.

### Additional notes

- The driver does not need to create SCHED_DEADLINE tasks — the bug is in the internal bandwidth accounting state that is set at boot.
- No cgroup configuration is needed.
- No topology configuration is needed beyond ensuring multiple CPUs.
- The detection is fully deterministic — the bug manifests on every boot of the buggy kernel with >1 CPU.
- kSTEP has full access to `cpu_rq(cpu)->dl` via `kernel/sched/sched.h` internals, so no framework extensions are needed.
- The driver should check multiple CPUs to confirm the pattern. CPU 0 (the boot CPU) is most likely to show the worst discrepancy since it was the only CPU online when initialization first occurred.
