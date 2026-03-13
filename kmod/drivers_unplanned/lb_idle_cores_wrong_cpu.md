# LB: has_idle_cores Flag Cleared on Wrong CPU's LLC

**Commit:** `02dbb7246c5bbbbe1607ebdc546ba5c454a664b1`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.13-rc2
**Buggy since:** v5.12-rc1 (introduced by commit 9fe1f127b913 "sched/fair: Merge select_idle_core/cpu()")

## Bug Description

During CFS task wakeup, the scheduler calls `select_idle_sibling()` which in turn calls `select_idle_cpu()` to find an idle CPU within the target CPU's LLC (Last Level Cache) domain. When SMT is present and the LLC domain has its `has_idle_cores` flag set (via `sd_llc_shared->has_idle_cores`), the function enters the idle-core scanning path by calling `select_idle_core()` for each CPU in the domain, looking for an entire idle core rather than just an idle CPU.

If the idle-core scan completes without finding any idle core, `select_idle_cpu()` clears the `has_idle_cores` flag to prevent future callers from wastefully scanning for idle cores in a domain that has none. However, due to a copy-paste error introduced in commit 9fe1f127b913, the code clears the flag on the **current CPU's** (`this`) LLC domain rather than the **target CPU's** (`target`) LLC domain. When the waking CPU and the target CPU reside in different LLCs, this bug causes two distinct problems simultaneously: (1) the target's LLC retains a stale `has_idle_cores=true` flag, causing all subsequent wakeups targeting that domain to wastefully scan for idle cores that don't exist, and (2) the current CPU's LLC has its `has_idle_cores` flag incorrectly cleared, potentially preventing legitimate idle-core scanning in a domain that does have idle cores.

The bug was introduced when Mel Gorman's commit 9fe1f127b913 refactored `select_idle_core()` and `select_idle_cpu()` to merge them into a single loop. In the old code, `select_idle_core()` was a standalone function that correctly called `set_idle_cores(target, 0)` when no idle core was found. The merge moved the clearing logic into `select_idle_cpu()` but mistakenly used the `this` variable (current CPU) instead of `target`.

## Root Cause

In the refactored `select_idle_cpu()` function, two CPU variables are in play:

- `this` = `smp_processor_id()` — the CPU currently executing the wakeup path
- `target` — the CPU suggested by `select_idle_sibling()` as a candidate for task placement

The function reads `has_idle_cores` from the target CPU's LLC with `smt = test_idle_cores(target, false)`, and then scans CPUs in `sched_domain_span(sd) & p->cpus_ptr` for idle cores. The scanning loop iterates via `for_each_cpu_wrap(cpu, cpus, target)` and calls `select_idle_core(p, cpu, cpus, &idle_cpu)` for each candidate.

After the loop, if `has_idle_core` (the local `smt` variable) is true and no idle core was found (the loop exhausted without returning early), the code must clear the flag in the **target's** LLC to reflect that no idle cores exist there anymore. The buggy code reads:

```c
if (has_idle_core)
    set_idle_cores(this, false);
```

The `set_idle_cores()` function accesses `per_cpu(sd_llc_shared, cpu)->has_idle_cores` for the given CPU argument. When `this != target` and they are in different LLCs, `per_cpu(sd_llc_shared, this)` and `per_cpu(sd_llc_shared, target)` point to different `sched_domain_shared` structures. The write goes to the wrong one.

Before the refactor, the old `select_idle_core()` was a self-contained function that iterated over cores in the target's domain and, at the end, called `set_idle_cores(target, 0)` correctly. When the loop was merged into `select_idle_cpu()`, the `target` parameter was available but the developer used `this` instead, likely because `this` was the more prominent local variable used for `cpu_clock()` timing and `this_sd` lookups elsewhere in the function.

## Consequence

The observable impact is a performance degradation on SMT systems where the waking CPU and the target CPU are in different LLCs. There are two effects:

1. **Wasteful idle-core scanning on the target's LLC**: The target LLC's `has_idle_cores` flag remains `true` even when no idle cores exist. Every subsequent `select_idle_cpu()` call targeting that LLC enters the expensive `select_idle_core()` path, scanning all SMT siblings of every CPU in the domain. This scanning is O(CPUs × SMT_siblings) and inflates the `avg_scan_cost`, which in turn reduces the `nr` limit for future non-SMT scans (via `SIS_PROP`). On large systems (e.g., 2-socket servers with many cores per socket), this can add measurable latency to the task wakeup path, particularly impacting workloads like hackbench that are sensitive to wakeup overhead.

2. **Missed idle-core optimization on the current CPU's LLC**: The waking CPU's LLC has its `has_idle_cores` flag prematurely cleared. Even if that LLC genuinely has idle cores, subsequent wakeups will skip the idle-core scanning path and fall through to the per-CPU scanning path. This means tasks may be placed on a CPU that shares a core with a busy sibling instead of an entirely idle core, leading to increased SMT contention and lower throughput. The flag is only restored when `__update_idle_core()` detects an idle core during a subsequent tick, which may take milliseconds.

There is no crash, hang, or data corruption — the bug is purely a performance issue affecting task placement quality on multi-LLC SMT systems.

## Fix Summary

The fix is a single-character change: replacing `this` with `target` in the `set_idle_cores()` call after the idle-core scanning loop:

```c
if (has_idle_core)
-    set_idle_cores(this, false);
+    set_idle_cores(target, false);
```

This ensures that when `select_idle_cpu()` fails to find an idle core in the target's LLC domain, it clears the `has_idle_cores` flag on the correct `sched_domain_shared` structure — the one belonging to the target CPU's LLC, which was the domain actually scanned. The `this` CPU's LLC flag is left untouched, preserving its correct state.

The fix is correct and complete because: (a) The scanning is always over the target's LLC domain (`sched_domain_span(sd)` where `sd` is the domain containing `target`), so it is the target's flag that should be updated. (b) The `test_idle_cores(target, false)` at the top of the function already reads from the target's LLC, maintaining consistency. (c) The original pre-refactor code in `select_idle_core()` correctly used `target`, confirming this was a regression.

## Triggering Conditions

- **Kernel version**: v5.12-rc1 through v5.13-rc1 (commits 9fe1f127b913..02dbb7246c5b~1)
- **CONFIG_SCHED_SMT=y**: Required for the `has_idle_cores` mechanism to be compiled in.
- **Multi-LLC topology**: The waking CPU (`this`) and the target CPU (`target`) must be in different LLC domains. This typically requires a multi-socket or chiplet-based system where different groups of cores have separate L3 caches.
- **SMT enabled**: Physical cores must have multiple hardware threads (e.g., Intel Hyper-Threading or POWER SMT) so that the idle-core scanning path is meaningful.
- **has_idle_cores initially true on target's LLC**: At least one core in the target's LLC must have been fully idle recently, causing `__update_idle_core()` to set the flag.
- **All cores busy in target's LLC at scan time**: When `select_idle_cpu()` runs, no core in the target's LLC should be fully idle, so the scan fails and the flag-clearing path is reached.
- **Workload**: Any task wakeup that causes `select_idle_sibling()` to pick a target in a different LLC than the current CPU. This can happen with `wake_affine()` selecting a remote CPU, or when `prev_cpu` is in a different LLC. High-throughput producer-consumer workloads across sockets (e.g., hackbench, netperf) would trigger this most frequently.

The bug is deterministic whenever the above conditions are met — there is no race condition. Every wakeup that scans for idle cores cross-LLC will clear the wrong flag.

## Reproduce Strategy (kSTEP)

1. **WHY can this bug not be reproduced with kSTEP?**
   The kernel version is too old. The bug was introduced in v5.12-rc1 (commit 9fe1f127b913) and fixed in v5.13-rc2 (commit 02dbb7246c5bbbbe1607ebdc546ba5c454a664b1). kSTEP supports Linux v5.15 and newer only. By v5.15, this bug has already been fixed for over a year (the fix was merged in May 2021, v5.15 was released in October 2021). There is no kernel version within kSTEP's supported range that contains this bug.

2. **WHAT would need to be added to kSTEP to support this?**
   If kSTEP supported pre-v5.15 kernels, the bug could theoretically be reproduced. The required infrastructure is largely already present: kSTEP supports multi-CPU QEMU configurations, topology setup (for multi-LLC), task creation, and wakeup control. The main requirement would be extending kSTEP to support kernel builds from the v5.12-v5.13 range, which may involve adapting internal headers and APIs that changed between v5.12 and v5.15.

3. **The reason is kernel version too old (pre-v5.15).** The fix was merged into v5.13-rc2. kSTEP's minimum supported version is v5.15, which already includes this fix.

4. **Alternative reproduction methods outside kSTEP:**
   - **Direct kernel testing**: Build a v5.12 or v5.13-rc1 kernel with CONFIG_SCHED_SMT=y on a dual-socket SMT system. Run a cross-socket wakeup-heavy workload (e.g., `hackbench -g 4 -l 10000`) and observe via `perf stat` or tracing that `select_idle_cpu()` spends excessive time in the idle-core scanning path. Compare with the fixed kernel to see reduced scan times.
   - **Ftrace/tracepoint verification**: Use `trace_sched_wakeup` and add a custom tracepoint or `printk` at the `set_idle_cores()` call site to log which CPU's LLC is being cleared. On the buggy kernel, you would observe `set_idle_cores(this, false)` clearing the wrong LLC when `this` and `target` differ. On the fixed kernel, the correct LLC is cleared.
   - **Code inspection**: The bug is a straightforward variable name error (`this` vs `target`) that can be verified by static analysis of the function's intent and the domain of the scan operation.

5. **If kSTEP were extended to support v5.12 kernels**, the reproduction strategy would be:
   - Configure QEMU with ≥4 CPUs across 2 LLC domains (e.g., CPUs 0-1 in LLC0, CPUs 2-3 in LLC1), each with 2 SMT threads per core.
   - Use `kstep_topo_init()` / `kstep_topo_set_smt()` / `kstep_topo_set_mc()` to establish the topology.
   - Create tasks pinned to different LLCs so that wakeups cross LLC boundaries.
   - Use a callback on the wakeup path to observe which `sd_llc_shared` has `has_idle_cores` cleared.
   - Verify that on the buggy kernel, the flag is cleared on the wrong LLC, and on the fixed kernel, it is cleared on the correct LLC.
