# Core: nr_uninterruptible Integer Overflow Corrupts Load Average

**Commit:** `36569780b0d64de283f9d6c2195fd1a43e221ee8`
**Affected files:** kernel/sched/loadavg.c, kernel/sched/sched.h
**Fixed in:** v6.16-rc7
**Buggy since:** v5.14-rc1 (introduced by commit `e6fe3f422be1` "sched: Make multiple runqueue task counters 32-bit")

## Bug Description

The per-CPU runqueue field `rq->nr_uninterruptible` tracks the number of tasks that entered `TASK_UNINTERRUPTIBLE` sleep on that CPU minus the number that woke up on that CPU. Because a task can block on one CPU and wake up (via `try_to_wake_up`) on a different CPU, individual per-CPU values are not meaningful in isolation — only the global sum across all CPUs yields the correct count of currently uninterruptible tasks. This design is intentional and documented in a comment at the top of `kernel/sched/loadavg.c`.

Commit `e6fe3f422be1` ("sched: Make multiple runqueue task counters 32-bit") changed the type of `rq->nr_uninterruptible` from `unsigned long` (64 bits on 64-bit systems) to `unsigned int` (32 bits), under the incorrect assumption that per-runqueue counters cannot exceed 2^32. While the total number of tasks in the system is bounded, the per-CPU `nr_uninterruptible` value is *not* bounded by the task count, because it is a differential counter that accumulates over the entire system uptime.

In long-running systems, a CPU that frequently has tasks enter `TASK_UNINTERRUPTIBLE` sleep on it (which then get migrated and wake up elsewhere) will see its `nr_uninterruptible` grow monotonically without bound. When this value exceeds `INT_MAX` (2,147,483,647), the `(int)` cast in `calc_load_fold_active()` causes it to be interpreted as a large negative number, corrupting the system load average calculation.

Additionally, the commit changed the cast in `calc_load_fold_active()` from `(long)` to `(int)`, compounding the problem: even if the unsigned int wrapping was handled correctly by unsigned arithmetic, the signed cast truncates the value and produces an incorrect sign.

## Root Cause

The root cause is a type-size reduction that fails to account for the accumulating nature of `rq->nr_uninterruptible`. The counter is modified in two places:

1. **Increment in `__block_task()` (sched.h line ~2959):** When a task enters `TASK_UNINTERRUPTIBLE` sleep (and `p->sched_contributes_to_load` is true), `rq->nr_uninterruptible++` is executed on the CPU where the task is currently running.

2. **Decrement in `ttwu_do_activate()` (core.c line ~3655):** When a task wakes up from `TASK_UNINTERRUPTIBLE` sleep, `rq->nr_uninterruptible--` is executed on the CPU where the task is being woken up, which may be a *different* CPU than where it went to sleep (due to migration in `try_to_wake_up()`).

Because the increment and decrement can occur on different CPUs, a single CPU can accumulate an arbitrarily large positive `nr_uninterruptible` value over time. Consider a workload where tasks repeatedly:
1. Enter `TASK_UNINTERRUPTIBLE` on CPU 0 (e.g., waiting for I/O, mutex)
2. Get migrated to CPU 1 and wake up there

Each cycle increments CPU 0's `nr_uninterruptible` by 1 and decrements CPU 1's by 1. After billions of such cycles (feasible on a server running for weeks or months), CPU 0's `nr_uninterruptible` can exceed `UINT_MAX/2` (i.e., `INT_MAX`).

The critical code path in `calc_load_fold_active()` (loadavg.c line 83) performs:

```c
nr_active = this_rq->nr_running - adjust;
nr_active += (int)this_rq->nr_uninterruptible;  /* BUG: (int) cast */
```

When `nr_uninterruptible` is, say, `0x80000001` (2,147,483,649 as unsigned), the `(int)` cast yields `-2147483647`. This produces a wildly incorrect `nr_active`, which is then used to compute a `delta` that gets added to the global `calc_load_tasks` atomic counter, which feeds into the exponential moving average for `avenrun[]` (the system load averages reported by `/proc/loadavg` and `uptime`).

The `(long)` cast in the original code was correct: on 64-bit systems, `(long)` preserves the full unsigned value as a positive number, since `unsigned long` values up to `ULONG_MAX/2` fit in a `long`. The `(int)` cast truncates and sign-extends, producing negative numbers for any value above `INT_MAX`.

## Consequence

The observable impact is a **miscalculation of the system load average**. The load average values reported by `/proc/loadavg`, the `uptime` command, and any monitoring tools that read `avenrun[]` become wildly incorrect.

Specifically, when a CPU's `nr_uninterruptible` exceeds `INT_MAX`, the `(int)` cast makes it appear as a large negative number. This negative contribution to `nr_active` in `calc_load_fold_active()` causes `calc_load_tasks` to decrease dramatically, potentially driving the load average to zero or even causing it to oscillate erratically. Since load average is a key metric for system monitoring, alerting, and autoscaling, this can cause:
- Monitoring systems to fail to detect overloaded systems
- Autoscalers to under-provision resources, believing the system is idle
- System administrators to make incorrect capacity planning decisions
- Load-balancing decisions in distributed systems to route traffic incorrectly

This is not a crash or a hang, but a silent data corruption of a critical system metric. The bug is insidious because it only manifests on long-running systems with significant task migration between CPUs, making it very difficult to diagnose — the system appears to function normally, but the load average is simply wrong. The time to trigger depends on the workload intensity: a server handling thousands of I/O-bound tasks per second could reach `INT_MAX` in days to weeks.

## Fix Summary

The fix is straightforward and consists of two changes:

1. **In `kernel/sched/sched.h`:** The type of `rq->nr_uninterruptible` is changed back from `unsigned int` to `unsigned long`. This restores the original 64-bit storage on 64-bit systems, ensuring that the counter cannot practically overflow (it would take billions of years at reasonable task rates to overflow a 64-bit counter).

2. **In `kernel/sched/loadavg.c`:** The cast in `calc_load_fold_active()` is changed from `(int)` back to `(long)`. This ensures that the unsigned long value is correctly sign-extended when added to the `long nr_active` variable. Since the global sum of `nr_uninterruptible` across all CPUs is always a small positive number (the actual count of uninterruptible tasks), the individual per-CPU values — even if very large as unsigned — will produce correct results when summed in `long` arithmetic on 64-bit systems.

The fix is correct and complete because it fully reverts the problematic type change for `nr_uninterruptible`, recognizing that this counter is fundamentally different from the other counters that were changed to 32-bit in the same commit (`dl_nr_migratory`, `dl_nr_running`, `rt_nr_boosted`, etc.). Those other counters *are* bounded by the total number of tasks, but `nr_uninterruptible` is a differential counter that accumulates over time and is only meaningful as a global sum.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

- **Kernel version:** Any kernel from v5.14-rc1 through v6.16-rc6 that includes commit `e6fe3f422be1`.
- **Architecture:** 64-bit system (on 32-bit systems, `unsigned int` and `unsigned long` are both 32 bits, so the type change has no effect — but the `(int)` cast is still problematic if the counter wraps).
- **CPU count:** At least 2 CPUs, so that task migration between CPUs is possible.
- **Workload:** Tasks that repeatedly enter `TASK_UNINTERRUPTIBLE` sleep on one CPU and wake up on a different CPU. This happens naturally with I/O-bound workloads (disk I/O, network I/O, mutex contention) when the waking CPU differs from the blocking CPU due to load balancing in `try_to_wake_up()`.
- **Duration:** The system must run long enough for `nr_uninterruptible` on at least one CPU to exceed `INT_MAX` (2,147,483,647). This requires approximately 2 billion task-block-then-migrate-then-wake cycles on a single CPU. On a busy server with thousands of I/O operations per second, this could happen in days to weeks.
- **Task state:** Tasks must have `sched_contributes_to_load` set to true, which means they enter `TASK_UNINTERRUPTIBLE` (not `TASK_UNINTERRUPTIBLE | TASK_NOLOAD` and not `TASK_FROZEN`). Regular disk I/O, mutex blocking, and many kernel wait paths use plain `TASK_UNINTERRUPTIBLE`.

The bug is **deterministic** once the counter exceeds the threshold — it does not require any race condition or specific timing. The only uncertainty is how long it takes for the counter to reach the overflow point, which depends entirely on the workload characteristics.

For a kSTEP driver, the overflow can be simulated by directly setting `rq->nr_uninterruptible` to a value above `INT_MAX`, bypassing the need for billions of actual task migrations.

## Reproduce Strategy (kSTEP)

The strategy uses only natural task operations — block, migrate via affinity pin, and wake — to trigger unsigned underflow of `rq->nr_uninterruptible` on a target CPU, then reads internal scheduler state (never writes it) to detect whether the bug is present. The core mechanism exploits the differential-counter design of `nr_uninterruptible`: blocking a task increments the counter on the blocking CPU, while waking the same task decrements it on the waking CPU. By forcing these to occur on different CPUs, we create an unsigned underflow on the waking CPU (0 → UINT_MAX on the buggy kernel, 0 → ULONG_MAX on the fixed kernel). The type difference is detectable by reading back the wrapped value and by checking `sizeof`.

### Step-by-step plan:

1. **Topology and boot setup:** Configure QEMU with at least 3 CPUs (`-smp 3`): CPU 0 is reserved for the kSTEP driver, CPU 1 serves as the "blocking" CPU, and CPU 2 serves as the "waking" CPU. No special topology, NUMA configuration, or boot parameters are needed beyond the CPU count — a flat SMP arrangement suffices. The driver does not call `kstep_topo_init()` since no custom topology is required.

2. **Import symbols for read-only observation:** Use `KSYM_IMPORT` to import the following kernel symbols for observation only — no direct writes to any of these:
   - `calc_load_tasks` (`atomic_long_t` in `kernel/sched/loadavg.c`) — the global load counter fed by `calc_load_fold_active()`.
   - `avenrun` (`unsigned long[3]` in `kernel/sched/loadavg.c`) — the 1/5/15-minute load averages.
   - `calc_load_fold_active` (function) — imported so we can call it to observe its return value after the natural state setup. Calling this kernel function is permitted; it reads `rq->nr_uninterruptible` and `rq->nr_running` and updates `rq->calc_load_active` as a side effect through the kernel's own code path, which is distinct from a direct field write by the driver.

   The per-CPU runqueue is accessed via the `cpu_rq()` macro already available through `internal.h`. All field accesses on the runqueue (`nr_uninterruptible`, `nr_running`, `calc_load_active`) are reads only.

3. **Record baseline state:** Before any task manipulation, record and log:
   - `cpu_rq(1)->nr_uninterruptible` and `cpu_rq(2)->nr_uninterruptible` (expected to be 0 or near-zero at boot).
   - `sizeof(cpu_rq(1)->nr_uninterruptible)` — this compile-time constant is the most direct indicator of the bug: **4 bytes** means the field is `unsigned int` (buggy), **8 bytes** means `unsigned long` (fixed). Log this value prominently.
   - `atomic_long_read(&calc_load_tasks)` and `avenrun[0]` for load average baseline.

4. **Create and position the test task:** Call `kstep_task_create()` to create a CFS task. Pin it to CPU 1 with `kstep_task_pin(task, 1, 1)`. Run `kstep_tick_repeat(5)` to allow the scheduler to place and run the task on CPU 1. Verify by reading `cpu_rq(1)->nr_running` that the task is on CPU 1. This task will be the vehicle for the block-migrate-wake cycle.

5. **Execute the block-migrate-wake cycle to trigger underflow:** This is the core of the reproduction. The cycle is performed in three phases, and can be repeated N times (e.g., N = 100) for amplification:

   **Phase A — Block on CPU 1:** Call `kstep_task_block(task)`. This puts the task into `TASK_UNINTERRUPTIBLE` sleep. The kernel's `__block_task()` function checks `p->sched_contributes_to_load` (which is true for plain `TASK_UNINTERRUPTIBLE`, as opposed to `TASK_UNINTERRUPTIBLE | TASK_NOLOAD`) and increments `cpu_rq(1)->nr_uninterruptible`. The task is dequeued from CPU 1's runqueue. Run `kstep_tick()` to ensure the state transition completes.

   **Phase B — Change affinity to CPU 2:** Call `kstep_task_pin(task, 2, 2)` to restrict the task's CPU affinity to only CPU 2. Since the task is currently sleeping (not on any runqueue), this merely updates the task's `cpus_mask` — no migration occurs yet and no runqueue counters are touched.

   **Phase C — Wake on CPU 2:** Call `kstep_task_wakeup(task)`. The kernel's `try_to_wake_up()` selects CPU 2 as the target (the only CPU allowed by the affinity mask) and calls `ttwu_do_activate()` on CPU 2's runqueue. This function checks `p->sched_contributes_to_load` and decrements `cpu_rq(2)->nr_uninterruptible`. Since CPU 2's counter was 0 (or already wrapped from previous iterations), this causes an unsigned underflow: **on the buggy kernel**, `(unsigned int)0 - 1 = 0xFFFFFFFF`; **on the fixed kernel**, `(unsigned long)0 - 1 = 0xFFFFFFFFFFFFFFFF`. Run `kstep_tick()` to let the task settle on CPU 2.

   **Phase D — Repin to CPU 1 for next iteration:** Call `kstep_task_pin(task, 1, 1)` and `kstep_tick_repeat(3)` to migrate the task back to CPU 1 before the next cycle. This does not affect `nr_uninterruptible` (migration of a running/runnable task does not touch this counter).

6. **Observe the underflow — primary detection:** After completing N block-migrate-wake cycles, read `cpu_rq(2)->nr_uninterruptible` into an `unsigned long` variable. The key observation:
   - **On the buggy kernel (unsigned int field):** The field is 4 bytes. The value `(unsigned int)(0 - N)` wraps to `UINT_MAX - N + 1`. When read into an `unsigned long`, the compiler zero-extends it, yielding a value ≤ `UINT_MAX` (4,294,967,295). For N = 1, this is exactly `0xFFFFFFFF`.
   - **On the fixed kernel (unsigned long field):** The field is 8 bytes. The value `(unsigned long)(0 - N)` wraps to `ULONG_MAX - N + 1`, which is far larger than `UINT_MAX`. For N = 1, this is `0xFFFFFFFFFFFFFFFF` (18,446,744,073,709,551,615).

   The runtime test is: if the read-back value is `<= (unsigned long)UINT_MAX`, the field is `unsigned int` and the bug is present. If it exceeds `UINT_MAX`, the field is `unsigned long` and the fix is in place. This cleanly distinguishes buggy from fixed kernels using only a READ of the internal field after natural task operations created the state.

7. **Observe the counterpart — verify differential counter:** Also read `cpu_rq(1)->nr_uninterruptible`, which should equal N (the number of tasks that blocked on CPU 1 and were migrated away). Log both CPUs' values and verify that their unsigned sum equals 0 (mod 2^32 on buggy, mod 2^64 on fixed), confirming the differential counter property: the global sum is correct (no tasks are currently uninterruptible) even though individual per-CPU values have wrapped.

8. **Verify load average impact via calc_load_fold_active:** Import and call `calc_load_fold_active(cpu_rq(2), 0)` to observe the delta it computes for CPU 2. The function reads `cpu_rq(2)->nr_uninterruptible` and casts it via `(int)` (buggy) or `(long)` (fixed):
   - **For small N (e.g., N = 100):** Both casts produce `-N` (i.e., `-100`), which is mathematically correct. The two's complement representation of small negative values is identical whether the intermediate type is `int` or `long`. This demonstrates that the code path functions correctly for small differential values — the bug is latent.
   - **Extrapolation to production failure:** Log the computed delta alongside a manual computation of what the `(int)` cast would produce if `cpu_rq(1)->nr_uninterruptible` had reached `0x80000010` (as it would after ~2.15 billion block-migrate cycles on a long-running server): `(int)0x80000010 = -2147483632` versus the correct `(long)0x80000010 = 2147483664`. This demonstrates the silent corruption that occurs in production after days-to-weeks of uptime with migrating I/O-bound workloads.

9. **Trigger periodic load average recalculation:** Run `kstep_tick_repeat(5 * HZ + 10)` to trigger at least one full `LOAD_FREQ` cycle, causing the kernel's periodic timer to call `calc_load_fold_active()` on each CPU and update `calc_load_tasks` and `avenrun[]`. Read these values after the tick sequence and compare to baseline. For the small N used in the test, both buggy and fixed kernels produce correct load averages — but the sizeof and value-range checks already identified the vulnerable type.

10. **Pass/fail criteria — three independent checks:**
    - **Check 1 (sizeof, compile-time type detection):** `sizeof(cpu_rq(1)->nr_uninterruptible)`. If `== 4` → `unsigned int` → bug present → `kstep_fail("nr_uninterruptible is unsigned int (%zu bytes), vulnerable to overflow", sz)`. If `== 8` → `unsigned long` → bug fixed → `kstep_pass(...)`.
    - **Check 2 (value-range, runtime underflow detection):** Read `cpu_rq(2)->nr_uninterruptible` after the block-migrate-wake cycles. If the value `<= (unsigned long)UINT_MAX` → wrapping is bounded to 32 bits → bug present → `kstep_fail(...)`. If the value `> (unsigned long)UINT_MAX` → 64-bit wrapping → fixed → `kstep_pass(...)`.
    - **Check 3 (cast simulation, impact projection):** Manually compute `(int)(unsigned int)nr_unintr_cpu1` and `(long)nr_unintr_cpu1` for CPU 1's accumulated positive value. For the small test values, both casts agree. Log the projected divergence point (`INT_MAX + 1 = 0x80000000`) and the corresponding production time estimate. This is informational rather than a pass/fail gate, since the actual divergence requires infeasible cycle counts in a test.

    The driver should `kstep_fail()` if either Check 1 or Check 2 indicates the buggy type, and `kstep_pass()` if both confirm the fixed type.

11. **Kernel version guard:** Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)` since the bug was introduced in v5.14-rc1 (commit `e6fe3f422be1`). The driver is valid for all kernels from v5.14 through v6.16-rc6.

12. **Logging:** Output detailed diagnostics at each stage:
    - `sizeof(rq->nr_uninterruptible)` to show field type size (the single most important output).
    - CPU 1 and CPU 2's `nr_uninterruptible` values before and after the block-migrate-wake cycles, in both decimal and hexadecimal.
    - The number of cycles N performed and the resulting per-CPU counter values.
    - The unsigned sum of both CPUs' `nr_uninterruptible` values (should be 0 modulo the type width, confirming the differential counter invariant).
    - The return value of `calc_load_fold_active(cpu_rq(2), 0)`.
    - The `(int)` vs `(long)` cast of a hypothetical `0x80000010` value, showing the production failure mode.
    - `calc_load_tasks` and `avenrun[0]` before and after the tick sequence.
    - Extrapolated time to reach `INT_MAX` based on a hypothetical 10,000-blocks/second production workload (~2.5 days).

### Expected behavior:

- **Buggy kernel (v5.14-rc1 to v6.16-rc6):** `sizeof(rq->nr_uninterruptible) == 4` (unsigned int). After N block-on-CPU1/wake-on-CPU2 cycles, CPU 2's `nr_uninterruptible` wraps to `UINT_MAX - N + 1` (bounded ≤ 4,294,967,295). CPU 1's value equals N. The `(int)` cast in `calc_load_fold_active()` produces correct results for the small test values, but the type-size and value-range checks identify the kernel as vulnerable. In production, after ~2.15 billion block-migrate cycles on a single CPU (reachable in days on busy servers), the `(int)` cast of the accumulated positive value would overflow `INT_MAX`, producing a large negative number that corrupts the system load average.

- **Fixed kernel (v6.16-rc7+):** `sizeof(rq->nr_uninterruptible) == 8` (unsigned long). After the same cycles, CPU 2's `nr_uninterruptible` wraps to `ULONG_MAX - N + 1` (far exceeding `UINT_MAX`). The `(long)` cast handles this correctly, and the 64-bit counter cannot practically overflow (would require billions of years of continuous operation). Both type-size and value-range checks confirm the fix.

This approach never writes to any internal scheduler field. All state changes to `nr_uninterruptible` occur through the kernel's own `__block_task()` and `ttwu_do_activate()` code paths, triggered indirectly by kSTEP's task block/wake/pin APIs. The driver only reads internal state for observation and verification.
