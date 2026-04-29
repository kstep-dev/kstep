# Core: Race in cpus_share_cache() During Sched Domain Rebuild

**Commit:** `42dc938a590c96eeb429e1830123fef2366d9c80`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.16-rc1
**Buggy since:** v3.3-rc1 (commit `518cd6234178` — "sched: Only queue remote wakeups when crossing cache boundaries")

## Bug Description

The function `cpus_share_cache(int this_cpu, int that_cpu)` determines whether two CPUs share a last-level cache (LLC) by comparing `per_cpu(sd_llc_id, this_cpu)` with `per_cpu(sd_llc_id, that_cpu)`. This function is called in the `try_to_wake_up()` hot path via `ttwu_queue_cond()` to decide whether a wakeup should be queued on a remote CPU's wake list (to avoid cross-cache-domain data accesses) or handled locally.

The `sd_llc_id` per-CPU variable is updated by `update_top_cache_domain()` in `kernel/sched/topology.c`, which is called during scheduling domain rebuilds triggered by `partition_sched_domains_locked()`. These rebuilds occur during cpuset changes, CPU hotplug events, and other topology reconfiguration operations. Critically, no lock protects the reads of `sd_llc_id` in `cpus_share_cache()` against the writes in `update_top_cache_domain()`.

When `cpus_share_cache()` is called with `this_cpu == that_cpu` (i.e., checking whether a CPU shares cache with itself, which should trivially be true), a data race with a concurrent `update_top_cache_domain()` can cause the function to return `false`. This happens because the two `per_cpu(sd_llc_id, X)` reads in `cpus_share_cache()` are not atomic — the value can change between the first and second read if another CPU writes to `sd_llc_id[X]` at exactly the wrong time.

The consequence is that `ttwu_queue_cond()` incorrectly returns `true` (indicating the wakeup should use the remote wake list), and `ttwu_queue_wakelist()` then hits its `WARN_ON_ONCE(cpu == smp_processor_id())` sanity check, since queuing a local CPU on the remote wake list is invalid.

## Root Cause

The root cause is a data race between two concurrent operations on the per-CPU variable `sd_llc_id`:

**Reader: `cpus_share_cache()` in `kernel/sched/core.c`**
```c
bool cpus_share_cache(int this_cpu, int that_cpu)
{
    return per_cpu(sd_llc_id, this_cpu) == per_cpu(sd_llc_id, that_cpu);
}
```
When called with `this_cpu == that_cpu == X`, this performs two separate memory reads of `per_cpu(sd_llc_id, X)`. These reads are not atomic and there is no barrier between them.

**Writer: `update_top_cache_domain()` in `kernel/sched/topology.c`**
```c
static void update_top_cache_domain(int cpu)
{
    struct sched_domain *sd;
    int id = cpu;
    // ...
    sd = highest_flag_domain(cpu, SD_SHARE_PKG_RESOURCES);
    if (sd) {
        id = cpumask_first(sched_domain_span(sd));
    }
    // ...
    per_cpu(sd_llc_id, cpu) = id;
    // ...
}
```
This function is called from `cpu_attach_domain()`, which is called from `detach_destroy_domains()` during `partition_sched_domains_locked()`. When scheduling domains are being torn down (e.g., when a NULL domain is attached), `sd` is NULL, so `id` defaults to `cpu`. If the previous value of `sd_llc_id[cpu]` was different (e.g., 0, which is the id of the first CPU in the old LLC group), the write changes the value.

The race proceeds as follows on the buggy kernel:
1. CPU 1 is performing a wakeup via `try_to_wake_up()` and calls `cpus_share_cache(X, X)` (where X is both the waker's and wakee's CPU).
2. The first read: `per_cpu(sd_llc_id, X)` returns the old value (e.g., 0 — the LLC group leader).
3. CPU 2 is executing `partition_sched_domains_locked()` → `detach_destroy_domains()` → `cpu_attach_domain(NULL, ..., X)` → `update_top_cache_domain(X)`, which writes `per_cpu(sd_llc_id, X) = X` (since `sd` is NULL, `id` defaults to `cpu`).
4. The second read: `per_cpu(sd_llc_id, X)` now returns the new value (X).
5. The comparison `0 == X` is false (for X > 0), so `cpus_share_cache()` returns `false`.

This false return causes `ttwu_queue_cond()` to return `true` (via the `!cpus_share_cache(smp_processor_id(), cpu)` check), and subsequently `ttwu_queue_wakelist()` hits `WARN_ON_ONCE(cpu == smp_processor_id())`.

The `cpus_share_cache()` function is called from `ttwu_queue_cond()` in two contexts:
1. **Direct from `try_to_wake_up()`**: When `p->on_cpu == 1`, `ttwu_queue_wakelist(p, task_cpu(p), wake_flags | WF_ON_CPU)` is called. Here `cpu = task_cpu(p)` and `smp_processor_id()` is the waker's CPU.
2. **Via `ttwu_queue()`**: After `select_task_rq()` picks the target CPU, `ttwu_queue(p, cpu, wake_flags)` → `ttwu_queue_wakelist(p, cpu, wake_flags)` is called. If the target CPU is the same as the current CPU, `cpu == smp_processor_id()`.

In both paths, if `cpu == smp_processor_id()` (a local wakeup), `cpus_share_cache(X, X)` should trivially return `true`. The race makes it return `false`, leading to the invalid wake list queuing attempt.

## Consequence

The immediate observable consequence is a kernel `WARN_ON_ONCE` firing in `ttwu_queue_wakelist()`:
```c
static bool ttwu_queue_wakelist(struct task_struct *p, int cpu, int wake_flags)
{
    if (sched_feat(TTWU_QUEUE) && ttwu_queue_cond(cpu, wake_flags)) {
        if (WARN_ON_ONCE(cpu == smp_processor_id()))
            return false;
        // ...
    }
    return false;
}
```

The `WARN_ON_ONCE` is a safeguard: when it fires, it returns `false` (preventing the task from actually being queued on the local CPU's remote wake list), so the fallback path handles the wakeup correctly. Without this safeguard, queuing a task on the local CPU's wake list via `__smp_call_single_queue()` would send an IPI to the current CPU, potentially causing a hang or data corruption.

In practice, the bug manifests as a kernel warning in dmesg with a stack trace through `ttwu_queue_wakelist` → `ttwu_queue_cond` → `cpus_share_cache`. The warning was reported by Jing-Ting Wu from MediaTek, likely observed on mobile SoC platforms where cpuset configuration changes are frequent (e.g., during CPU frequency/power management transitions that modify scheduling domains).

The bug does not cause a crash or data corruption due to the `WARN_ON_ONCE` guard, but it indicates an incorrect scheduling decision path was entered. In a kernel without the `WARN_ON_ONCE` check, the consequences would be more severe — sending an IPI to the local CPU and potentially corrupting the wake list data structures.

## Fix Summary

The fix adds an early return to `cpus_share_cache()` that bypasses the `sd_llc_id` comparison when the two CPU arguments are the same:

```c
bool cpus_share_cache(int this_cpu, int that_cpu)
{
    if (this_cpu == that_cpu)
        return true;

    return per_cpu(sd_llc_id, this_cpu) == per_cpu(sd_llc_id, that_cpu);
}
```

This is correct because a CPU trivially shares cache with itself — there is no conceivable scenario where a CPU should be considered in a different cache domain from itself. By returning `true` before reading `sd_llc_id`, the race window is completely eliminated for the `this_cpu == that_cpu` case.

The fix is minimal, safe, and has zero performance impact (comparing two integers is cheaper than two `per_cpu` lookups). It does not address the theoretical race when `this_cpu != that_cpu` (where a stale `sd_llc_id` could produce an incorrect result), but that case is benign — a momentarily wrong cache-sharing decision between different CPUs only affects whether the wake list or direct activation path is used, which is a performance optimization rather than a correctness requirement.

The fix was reviewed by Valentin Schneider and Vincent Guittot and merged by Peter Zijlstra for v5.16-rc1.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger the bug:

1. **SMP system with 2+ CPUs**: The race requires at least two CPUs — one performing a wakeup and another performing a scheduling domain rebuild. The `cpus_share_cache` function is only compiled and used in `CONFIG_SMP` builds.

2. **A wakeup where `smp_processor_id() == cpu`**: The waker must be on the same CPU as the wakee's target CPU. This happens when:
   - A task on CPU X wakes another task that was last running on CPU X (and `select_task_rq` keeps it on CPU X).
   - An interrupt handler on CPU X wakes a task associated with CPU X.
   - A kthread on CPU X wakes a blocked task on CPU X.

3. **Concurrent scheduling domain rebuild**: `partition_sched_domains_locked()` must be executing on another CPU at the same time, specifically running `update_top_cache_domain()` for the CPU involved in the wakeup. Domain rebuilds are triggered by:
   - `cpuset` configuration changes (adding/removing CPUs from cpusets).
   - CPU hotplug events (online/offline).
   - `sched_domain_topology` changes.
   - `rebuild_sched_domains()` calls.

4. **Precise instruction-level timing**: The write to `per_cpu(sd_llc_id, X)` in `update_top_cache_domain()` must occur between the two reads of `per_cpu(sd_llc_id, X)` in `cpus_share_cache()`. The race window is extremely narrow — only 1-3 instructions between the two reads. This makes the bug very difficult to reproduce reliably, though it can occur on systems with frequent domain rebuilds (e.g., mobile platforms with aggressive power management).

5. **`sd_llc_id` value change**: The domain rebuild must actually change the `sd_llc_id` value for the target CPU. This happens when:
   - The CPU's LLC domain changes (e.g., from being part of a multi-CPU LLC group to being in a singleton domain).
   - Domains are detached (`cpu_attach_domain(NULL, ...)` sets `id = cpu` regardless of the old LLC group).

6. **`TTWU_QUEUE` sched feature enabled** (default on): The `ttwu_queue_wakelist()` function checks `sched_feat(TTWU_QUEUE)` before proceeding.

The probability of hitting this race in any single wakeup+rebuild overlap is extremely low due to the narrow timing window, but on busy systems with frequent topology changes, it can eventually manifest.

## Reproduce Strategy (kSTEP)

### Overview

This bug is a data race between `cpus_share_cache()` (called during task wakeup via `try_to_wake_up()`) and `update_top_cache_domain()` (called during scheduling domain rebuild). The race window is extremely narrow — only 1-3 instructions between the two `per_cpu(sd_llc_id, X)` reads in `cpus_share_cache()`. Reproducing this bug requires overlapping a local wakeup (where `smp_processor_id() == task_cpu(wakee)`) with a concurrent domain rebuild that transiently changes the `sd_llc_id` value for the target CPU. This strategy uses only public kernel APIs and kSTEP interfaces to trigger both the wakeup and the domain rebuild, relying on read-only access to internal scheduler state (`sd_llc_id`, `WARN_ON_ONCE` flag) solely for observation and verdict determination. No internal scheduler state is written.

### Strategy 1: Cpuset-Driven Domain Rebuild with Concurrent Kthread Wakeups (Primary)

This approach uses cpuset reconfiguration — the realistic trigger described in the original bug report — to drive scheduling domain rebuilds while kthreads bound to a target CPU perform local wakeups that traverse the `cpus_share_cache()` code path.

**Setup:**
1. Configure QEMU with 4 CPUs (CPU 0 for the driver, CPUs 1-3 for workload). Use boot parameters `idle=poll` to prevent CPUs from entering deep idle states, which improves timing determinism and keeps CPUs responsive to scheduling events. Optionally add `nosmt` to simplify the topology and make LLC domain changes more pronounced.
2. Create two cpuset cgroups that will be toggled to trigger domain rebuilds:
   ```c
   kstep_cgroup_create("groupA");
   kstep_cgroup_set_cpuset("groupA", "1-3");
   kstep_cgroup_create("groupB");
   kstep_cgroup_set_cpuset("groupB", "1-2");
   ```
3. Create 4-6 kthread pairs (waker and wakee), all bound to CPU 1 via `kstep_kthread_bind()` with a CPU 1-only mask. Start them with `kstep_kthread_start()`. The key requirement is that the waker runs on CPU 1 so that `__wake_up_sync()` inside `do_syncwakeup` calls `try_to_wake_up()` from CPU 1's context. Since the wakee was also blocked on CPU 1, `task_cpu(wakee) == 1` and `smp_processor_id() == 1`, producing the critical `cpus_share_cache(1, 1)` call.
4. Let all kthreads settle with `kstep_tick_repeat(5)`.

**Execution Loop (repeat 200+ iterations):**
1. Block all wakee kthreads with `kstep_kthread_block(wakee_i)` for each pair. This puts them into `TASK_INTERRUPTIBLE` sleep on their internal wait queues on CPU 1.
2. Wait for the wakees to actually deschedule: `kstep_tick_repeat(3)`.
3. For each waker/wakee pair, call `kstep_kthread_syncwake(waker_i, wakee_i)`. This is non-blocking from the driver's perspective — it sets the waker's action to `do_syncwakeup` and signals the change. The actual `__wake_up_sync()` call happens asynchronously when the waker kthread is next scheduled on CPU 1.
4. **Immediately** (without any intervening tick or sleep) toggle the cpuset configuration to trigger a domain rebuild:
   ```c
   kstep_cgroup_set_cpuset("groupA", "1-2");  // or toggle back to "1-3"
   ```
   This calls `partition_sched_domains_locked()` → `detach_destroy_domains()` → `cpu_attach_domain(NULL, ...)` → `update_top_cache_domain(1)` on CPU 0. During the detach phase, `sd_llc_id[1]` transiently changes from its LLC leader value (e.g., 0 or 1) to `1` (the CPU's own ID). During the subsequent domain rebuild, it may change back to the LLC leader ID. These transient value changes create the race window.
5. The goal: a waker on CPU 1 executing `__wake_up_sync()` → `try_to_wake_up()` → `ttwu_queue_cond()` → `cpus_share_cache(1, 1)` overlaps with `update_top_cache_domain(1)` writing `per_cpu(sd_llc_id, 1)` between the two reads.
6. After each iteration, wait for kthreads to settle (`kstep_tick_repeat(3)`) and re-create the waker kthreads (since `kstep_kthread_syncwake` causes the waker to exit after performing the wakeup action). Alternate the cpuset configuration between `"1-3"` and `"1-2"` to ensure a different LLC partition on each iteration, maximizing the chance that `sd_llc_id[1]` actually changes value.

**Detection:**
Import the kernel's `sd_llc_id` per-CPU variable (read-only) via `KSYM_IMPORT(sd_llc_id)` and periodically read `per_cpu(sd_llc_id, 1)` from the driver on CPU 0 to confirm that values are actually changing during cpuset toggles. This read-only observation validates that the race window exists even if the WARN does not fire. For the actual bug signal, check the kernel log for the `WARN_ON_ONCE` in `ttwu_queue_wakelist()`. The `kstep_print_sched_debug()` function can dump relevant scheduler state after each batch of iterations.

### Strategy 2: KCSAN-Assisted Data Race Detection

The Kernel Concurrency Sanitizer (KCSAN) can detect the data race between `cpus_share_cache()` and `update_top_cache_domain()` without requiring the race to produce an observable incorrect result. This transforms the problem from hitting a 1-3 instruction race window to simply having concurrent access — a much wider target.

**Kernel Build Configuration:**
Build the test kernel with `CONFIG_KCSAN=y` and `CONFIG_KCSAN_REPORT_ONCE_IN_MS=0` (to report every instance, not rate-limited). This enables compiler instrumentation that detects unsynchronized concurrent accesses to the same memory location where at least one is a write.

**Execution:**
Use the same setup as Strategy 1 — kthread wakeup pairs on CPU 1 with concurrent cpuset-driven domain rebuilds on CPU 0. The key difference is that KCSAN does not require the race to actually corrupt a result; it only requires the two accesses to overlap within KCSAN's sampling window. Since KCSAN uses a watchpoint-based approach (setting a watchpoint on one access and checking if another access hits the same address during a delay window), the detection probability per iteration is significantly higher than the probability of the two `per_cpu` reads in `cpus_share_cache()` being interleaved by a write.

**Detection:**
KCSAN reports appear in the kernel log as `BUG: KCSAN: data-race in cpus_share_cache / update_top_cache_domain`. On the **fixed kernel**, `cpus_share_cache(1, 1)` returns before reading `sd_llc_id`, so KCSAN will not report a race for the `this_cpu == that_cpu` case (the reads never happen). On the **buggy kernel**, the reads always occur and can race with the concurrent write. Parse `dmesg` output after each batch of iterations to check for KCSAN reports mentioning `cpus_share_cache`.

**Limitations:**
KCSAN adds significant overhead (2-5x slowdown), which alters timing. However, since KCSAN detects the potential for a race rather than requiring the race to produce a wrong result, this is acceptable. The KCSAN approach provides a strong differential signal: the buggy kernel will report the race, the fixed kernel will not.

### Strategy 3: Topology API Rapid Rebuild with Maximized Wakeup Traffic

This is an alternative to the cpuset-driven approach that uses `kstep_topo_apply()` directly for domain rebuilds, combined with maximized kthread wakeup traffic.

**Setup:**
1. Configure QEMU with 4+ CPUs. Boot with `idle=poll`.
2. Define an initial multi-level topology using `kstep_topo_set_mc()`:
   ```c
   const char *mc[] = {"0-1", "0-1", "2-3", "2-3"};
   kstep_topo_set_mc(mc, 4);
   kstep_topo_apply();
   ```
   This places CPUs 0-1 in one LLC group (`sd_llc_id[0] = sd_llc_id[1] = 0`) and CPUs 2-3 in another (`sd_llc_id[2] = sd_llc_id[3] = 2`).
3. Create 6-8 kthread pairs bound to CPU 1. Start them and let them settle.

**Execution Loop (repeat 500+ iterations):**
1. Block all wakees on CPU 1.
2. Wait briefly: `kstep_tick_repeat(2)`.
3. Issue `kstep_kthread_syncwake()` for all pairs in rapid succession.
4. **Immediately** call `kstep_topo_apply()`. This invokes `rebuild_sched_domains()` on CPU 0, which tears down and recreates all scheduling domains. During teardown, `detach_destroy_domains()` sets `sd_llc_id[1] = 1` (breaking the LLC group), and during rebuild, `sd_llc_id[1]` is restored to the LLC leader value (0). Each `kstep_topo_apply()` call thus creates two transient `sd_llc_id` changes for CPU 1.
5. Alternate the topology between two configurations on successive iterations:
   ```c
   // Even iterations: CPUs 0-1 share LLC
   const char *mc_a[] = {"0-1", "0-1", "2-3", "2-3"};
   // Odd iterations: Each CPU in its own LLC
   const char *mc_b[] = {"0", "1", "2", "3"};
   ```
   This ensures that `sd_llc_id[1]` changes between 0 and 1 across iterations, in addition to the transient changes during each rebuild.
6. Re-create waker kthreads after each iteration (they exit after syncwake).

**Rationale for high iteration count:**
The race window is ~1-3 instructions wide. With 6 kthread pairs generating wakeups concurrently and `kstep_topo_apply()` creating two `sd_llc_id` value transitions per call, each iteration provides approximately 12 "chances" for overlap. At 500 iterations, this yields ~6000 race opportunities. While each individual opportunity has a very low probability of hitting the exact instruction-level timing, the cumulative probability over thousands of attempts may produce a hit, particularly given QEMU's non-deterministic scheduling jitter.

### Strategy 4: Observational Verification of the Race Window

Even if the race is not hit (the WARN_ON_ONCE does not fire), the driver can verify that the preconditions for the race exist by observing internal state changes (read-only).

**Verification Steps:**
1. Import `sd_llc_id` via `KSYM_IMPORT(sd_llc_id)` (read-only pointer).
2. Import `cpus_share_cache` via `KSYM_IMPORT(cpus_share_cache)` (read-only pointer to call the function).
3. Before a domain rebuild, read `per_cpu(sd_llc_id, 1)` and record the value (e.g., 0).
4. Call `kstep_topo_apply()` to trigger a rebuild.
5. After the rebuild, read `per_cpu(sd_llc_id, 1)` again.
6. If the topology was changed between iterations (as in Strategy 3), the value will have changed, confirming that the race window exists — during the rebuild, `sd_llc_id[1]` transiently held a different value.
7. Call `KSYM_cpus_share_cache(1, 1)` from the driver (on CPU 0, so this doesn't trigger the race itself, but exercises the code path):
   - On the **fixed kernel**: The early return `if (this_cpu == that_cpu) return true;` fires immediately, returning `true` without reading `sd_llc_id`. This is always correct regardless of `sd_llc_id` state.
   - On the **buggy kernel**: The function reads `sd_llc_id[1]` twice. Since the driver runs on CPU 0 (single-threaded, no concurrent rebuild at this point), both reads see the same value and the function returns `true`. However, this confirms the code path difference — the buggy kernel performs two reads where the fixed kernel performs zero.
8. To highlight the difference, call `KSYM_cpus_share_cache(1, 1)` during a topology change (i.e., from a callback or in between topology operations). While the driver on CPU 0 calls `kstep_topo_apply()` synchronously, the rebuild itself may involve per-CPU work that overlaps with the function call.

**This strategy cannot deterministically trigger the bug**, but it provides strong evidence that the race window exists and that the fix eliminates it by bypassing the vulnerable code path entirely.

### Boot Parameters and CONFIG Options

The following boot parameters and kernel configuration options improve reproducibility:

**Boot parameters (via run.py):**
- `idle=poll`: Prevents CPUs from entering idle states, keeping them in a tight poll loop. This improves timing determinism and ensures CPU 1 responds quickly to scheduling events.
- `nosmt`: Disables SMT (simultaneous multithreading), simplifying the scheduling domain hierarchy and making LLC domain changes more pronounced.
- `nohz=off`: Disables tickless mode, ensuring regular timer interrupts on all CPUs. This adds more scheduling activity that could overlap with domain rebuilds.

**CONFIG options:**
- `CONFIG_KCSAN=y`: Enables the Kernel Concurrency Sanitizer for Strategy 2.
- `CONFIG_SCHED_DEBUG=y`: Enables `/sys/kernel/debug/sched/` interfaces and additional scheduler debug output, useful for observing domain state during the test.
- `CONFIG_DEBUG_PREEMPT=y`: Adds preemption debugging, which may widen timing windows slightly.
- `CONFIG_PROVE_LOCKING=y`: While not directly related, lockdep annotations can detect related domain-rebuild locking issues.

### kSTEP Changes Needed

No fundamental changes are needed. Minor helpful extensions:
1. **A non-exiting syncwake action**: Currently `kstep_kthread_syncwake` causes the waker to exit after performing the wakeup. A variant that returns the waker to spinning state (`do_spin`) instead of exiting would allow reusing kthreads across iterations without recreating them, significantly reducing per-iteration overhead and enabling higher iteration counts.
2. **A tight wake/block cycle action**: A kthread action that rapidly and repeatedly blocks the wakee and wakes it in a tight loop would maximize the number of `try_to_wake_up()` calls per unit time without requiring driver intervention for each cycle. This could increase race opportunities by orders of magnitude.
3. **Warning detection helper**: A `kstep_check_warn()` function that reads the kernel log buffer (`printk_ringbuffer`) and checks for `WARN_ON_ONCE` messages matching a pattern (e.g., `"ttwu_queue_wakelist"`) would enable automated pass/fail without external log parsing.
4. **KCSAN report parser**: For Strategy 2, a helper to parse KCSAN reports from the log buffer and match them against specific function names would automate the KCSAN-based detection.

### Pass/Fail Criteria

- On the **buggy kernel**: The `WARN_ON_ONCE` in `ttwu_queue_wakelist()` may fire (probabilistic — if it fires even once, the bug is confirmed). With `CONFIG_KCSAN=y`, KCSAN should report a data race in `cpus_share_cache` / `update_top_cache_domain`. Use `kstep_pass()` if either signal is detected.
- On the **fixed kernel**: The `WARN_ON_ONCE` should never fire, because `cpus_share_cache(1, 1)` always returns `true` due to the early exit, regardless of `sd_llc_id` state. KCSAN should not report a race for the `this_cpu == that_cpu` case (the `sd_llc_id` reads are bypassed). Use `kstep_pass()` if no warning/race is detected after all iterations complete.
- Use `kstep_fail()` if behavior is identical on both kernels (i.e., no WARN on buggy and no WARN on fixed — the race was not hit and KCSAN was not used).

### Expected Behavior

| Kernel | cpus_share_cache(X,X) during domain rebuild | WARN_ON_ONCE fires? | KCSAN data-race report? |
|--------|---------------------------------------------|---------------------|-------------------------|
| Buggy  | May return `false` (race-dependent)         | Yes (probabilistic) | Yes (high probability)  |
| Fixed  | Always returns `true` (early exit)          | Never               | No (reads bypassed)     |
