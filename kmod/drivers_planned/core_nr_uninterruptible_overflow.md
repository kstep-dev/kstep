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

The strategy is to directly set `rq->nr_uninterruptible` to a value exceeding `INT_MAX` on one CPU, then trigger the load average calculation and observe that the result is incorrect on the buggy kernel but correct on the fixed kernel.

### Step-by-step plan:

1. **Topology setup:** Configure QEMU with at least 2 CPUs. No special topology is needed — a simple SMP configuration suffices. Call `kstep_topo_init()` if needed for a basic 2-CPU setup.

2. **Access internal state:** Use `KSYM_IMPORT` to access the following symbols:
   - `cpu_rq()` macro (already available via `internal.h`) to get the per-CPU runqueue.
   - `calc_load_tasks` (an `atomic_long_t` in loadavg.c) to observe the load average input.
   - `avenrun` (an `unsigned long[3]` in loadavg.c) to observe the computed load averages.
   - `calc_load_fold_active` function — or alternatively, just set the state and let the periodic timer invoke it.

3. **Create a baseline:** Before modifying any state, create one or two CFS tasks with `kstep_task_create()` and let them run for a few ticks (`kstep_tick_repeat(100)`) to establish a stable load average baseline. Record the initial values of `avenrun[0]`, `avenrun[1]`, and `avenrun[2]`, and `atomic_long_read(&calc_load_tasks)`.

4. **Set up the overflow condition:** Directly write to `cpu_rq(1)->nr_uninterruptible` and set it to a value above `INT_MAX`, such as `0x80000010` (2,147,483,664). This simulates the state of a CPU that has had billions of tasks block on it and wake up elsewhere over a long uptime.

5. **Trigger load average recalculation:** Run `kstep_tick_repeat(N)` where N is large enough to trigger at least one `calc_load_fold_active()` invocation. The load average is recalculated every `LOAD_FREQ` (5 * HZ + 1) ticks. So running for at least 5 * HZ ticks (e.g., `kstep_tick_repeat(5 * HZ + 10)`) should trigger at least one recalculation cycle.

6. **Observe the result:** After the ticks, read `atomic_long_read(&calc_load_tasks)` and `avenrun[0]`.
   - **On the buggy kernel:** `calc_load_fold_active()` casts `nr_uninterruptible` (which is `unsigned int` = `0x80000010`) via `(int)`, yielding `-2147483632`. This massive negative value is added to `nr_active`, producing a very negative delta that drives `calc_load_tasks` deeply negative. The `avenrun[]` values will drop to zero or be wildly incorrect.
   - **On the fixed kernel:** `calc_load_fold_active()` casts `nr_uninterruptible` (which is `unsigned long` = `0x80000010`) via `(long)`, yielding `2147483664`. This correct positive value is added to `nr_active`, producing a large positive delta that correctly reflects in `calc_load_tasks`. The `avenrun[]` values will be extremely high (reflecting billions of "active" tasks), which is the correct behavior given the artificial state.

7. **Pass/fail criteria:**
   - Read `calc_load_tasks` after the tick sequence. If it is negative or zero when `nr_uninterruptible` was set to a large positive value, the bug is present → `kstep_fail("calc_load_tasks is %ld, expected large positive", val)`.
   - If `calc_load_tasks` is a large positive value (reflecting the artificial nr_uninterruptible), the bug is fixed → `kstep_pass("calc_load_tasks correctly reflects nr_uninterruptible=%lu", nr_unintr)`.

8. **Alternative approach — observe via delta:** Instead of reading absolute values, we can focus on the delta returned by `calc_load_fold_active()` directly:
   - Use `KSYM_IMPORT(calc_load_fold_active)` to import the function.
   - Set `rq->nr_uninterruptible = 0x80000010` and `rq->calc_load_active = 0` on a target CPU's rq.
   - Call `calc_load_fold_active(rq, 0)` and check the return value.
   - On the buggy kernel, the delta will be negative (approximately -2147483632).
   - On the fixed kernel, the delta will be positive (approximately 2147483664).
   - This is more precise and doesn't require waiting for periodic timer invocation.

9. **Kernel version guard:** Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)` since the bug was introduced in v5.14-rc1 (commit `e6fe3f422be1`). The driver is valid for all kernels from v5.14 through v6.16-rc6.

10. **Logging:** Add detailed logging:
    - Print the value of `nr_uninterruptible` before and after setting it.
    - Print `sizeof(cpu_rq(1)->nr_uninterruptible)` to confirm the type size (4 bytes on buggy, 8 bytes on fixed).
    - Print the result of the `(int)` vs `(long)` cast to show the truncation.
    - Print `calc_load_tasks` and `avenrun[0]` before and after triggering the recalculation.

### Expected behavior:

- **Buggy kernel (v5.14-rc1 to v6.16-rc6):** `sizeof(rq->nr_uninterruptible) == 4`. Setting it to `0x80000010` and calling `calc_load_fold_active()` returns a negative delta (approximately `-2147483632`). The load average is driven to zero or becomes erratic.

- **Fixed kernel (v6.16-rc7+):** `sizeof(rq->nr_uninterruptible) == 8`. Setting it to `0x80000010` and calling `calc_load_fold_active()` returns a correct large positive delta (approximately `2147483664`). The load average correctly reflects the artificially high uninterruptible count.

The simplest and most reliable approach is to use the direct function call method (step 8), as it avoids timing dependencies and directly tests the buggy code path. The type-size check (`sizeof`) can serve as an additional confirmation.
