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

This bug is a data race between `cpus_share_cache()` (called during task wakeup) and `update_top_cache_domain()` (called during scheduling domain rebuild). The core challenge for kSTEP reproduction is hitting the extremely narrow race window where `sd_llc_id[X]` changes between two consecutive reads.

### Strategy 1: Probabilistic Concurrent Stress

This approach creates concurrent wakeup traffic on a non-driver CPU while the driver CPU triggers repeated topology rebuilds.

**Setup:**
1. Configure QEMU with at least 3 CPUs (CPU 0 for driver, CPU 1 and CPU 2 for workload).
2. No special topology beyond the default (all CPUs in one LLC domain initially).

**Task Creation:**
1. Create a pair of kthreads: `waker` and `wakee`, both bound to CPU 1 via `kstep_kthread_bind()`.
2. Start both kthreads with `kstep_kthread_start()`.
3. Let them settle with a few `kstep_tick()` calls.

**Execution Loop (repeat many times):**
1. Block `wakee` with `kstep_kthread_block(wakee)` — this makes `wakee` call `wait_event()` → `schedule()` on CPU 1.
2. Wait briefly for `wakee` to actually block (a few `kstep_tick()` calls).
3. Call `kstep_kthread_syncwake(waker, wakee)` — this is non-blocking; it sets `waker`'s action to `do_syncwakeup`, which will call `__wake_up_sync()` from CPU 1's context.
4. **Immediately** call `kstep_topo_apply()` on the driver (CPU 0). This calls `rebuild_sched_domains()` → `partition_sched_domains_locked()` → `update_top_cache_domain()` for all CPUs, including CPU 1.
5. The goal: the wakeup on CPU 1 (step 3) and the domain rebuild on CPU 0 (step 4) overlap, hitting the race window in `cpus_share_cache(1, 1)`.
6. After each iteration, re-create `waker` and `wakee` (since `kstep_kthread_syncwake` causes the waker to exit).

**Detection:**
- Use `KSYM_IMPORT` to import the `__warned` variable associated with the `WARN_ON_ONCE` in `ttwu_queue_wakelist()`, or check kernel log (`dmesg`) for the warning string.
- Alternatively, import `sd_llc_id` via `KSYM_IMPORT` and read `per_cpu(sd_llc_id, 1)` in a tight loop on CPU 0 during topology changes to confirm that value changes are observable.

**Limitations:** The race window is ~1-3 instructions wide. Even with many iterations, hitting it is probabilistic. This strategy may require thousands of iterations or may not reliably trigger the bug in QEMU (which has lower concurrency resolution than real hardware).

### Strategy 2: Deterministic Verification via sd_llc_id Manipulation

This approach directly verifies the consequence of the race by manipulating `sd_llc_id` to create the state that the race would produce, then observing the incorrect `cpus_share_cache` behavior.

**Setup:**
1. Configure QEMU with 2+ CPUs.
2. Import `sd_llc_id` via `KSYM_IMPORT`:
   ```c
   KSYM_IMPORT(sd_llc_id);
   ```

**Verification Steps:**
1. Read the initial value of `per_cpu(sd_llc_id, 1)` (e.g., it's 0 if CPU 0 is the LLC group leader).
2. On the buggy kernel: observe that `cpus_share_cache(1, 1)` reads `sd_llc_id[1]` twice. If we could insert a write between those reads, it would return false.
3. Since we can't directly control instruction-level timing, instead verify the fix behavior:
   - On the **fixed kernel**: Call `cpus_share_cache(1, 1)` after modifying `per_cpu(sd_llc_id, 1)` to an arbitrary value. The fix's early return (`this_cpu == that_cpu → true`) ensures it always returns `true` regardless of `sd_llc_id` value.
   - On the **buggy kernel**: Call `cpus_share_cache(1, 1)` after modifying `per_cpu(sd_llc_id, 1)`. Both reads see the same (wrong) value, so it still returns `true`. This doesn't demonstrate the race, but confirms the code path difference.

**Enhancement — Widening the race window:**
To make the probabilistic approach more reliable, a minor kSTEP extension could add a wrapper that:
1. Patches `cpus_share_cache` at runtime (via `KSYM_IMPORT` + `text_poke()` or ftrace) to call a custom function that inserts a `udelay()` or `cpu_relax()` loop between the two `per_cpu` reads.
2. This widens the race window from ~nanoseconds to ~microseconds, making the concurrent approach (Strategy 1) far more likely to hit the race.

### Strategy 3: Kthread-Driven Concurrent Stress (Recommended)

This is a refined version of Strategy 1 that maximizes the number of race attempts.

**Setup:**
1. Configure QEMU with 3+ CPUs.
2. Create 6-8 kthreads bound to CPU 1 (3-4 waker/wakee pairs).

**Execution:**
1. Block all wakee kthreads.
2. For each waker/wakee pair, call `kstep_kthread_syncwake(waker, wakee)` — the wakers will wake their wakees from CPU 1 context.
3. **Immediately** call `kstep_topo_apply()`.
4. Sleep briefly (`kstep_sleep()` or a few `kstep_tick()`).
5. Check for the warning.
6. Re-create kthreads for the next iteration.
7. Repeat 100+ times.

**Pass/Fail Criteria:**
- On the **buggy kernel**: The `WARN_ON_ONCE` in `ttwu_queue_wakelist()` may fire (probabilistic — if it fires even once, the bug is confirmed). Check via `dmesg` or `printk` detection.
- On the **fixed kernel**: The `WARN_ON_ONCE` should never fire, because `cpus_share_cache(1, 1)` always returns `true` due to the early exit, regardless of `sd_llc_id` state.
- Use `kstep_pass()` if the warning fires on the buggy kernel and doesn't fire on the fixed kernel. Use `kstep_fail()` if the behavior is the same on both.

### kSTEP Changes Needed

No fundamental changes are needed. Minor helpful extensions:
1. **A non-exiting syncwake action**: Currently `kstep_kthread_syncwake` causes the waker to exit after waking. A variant that returns the waker to spinning state (`do_spin`) instead of exiting would allow reusing kthreads across iterations without recreating them.
2. **A tight wake/block cycle action**: A kthread action that rapidly blocks and wakes another kthread in a loop would maximize race opportunities without driver intervention each iteration.
3. **Warning detection helper**: A `kstep_check_warn()` function that reads the kernel log buffer and checks for WARN_ON_ONCE messages matching a pattern.

### Expected Behavior

| Kernel   | cpus_share_cache(X,X) during domain rebuild | WARN_ON_ONCE fires? |
|----------|---------------------------------------------|---------------------|
| Buggy    | May return `false` (race-dependent)         | Yes (probabilistic) |
| Fixed    | Always returns `true` (early exit)          | Never               |
