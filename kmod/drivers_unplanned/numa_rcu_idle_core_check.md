# NUMA: Missing RCU Lock in update_numa_stats() Idle Core Check

**Commit:** `0621df315402dd7bc56f7272fae9778701289825`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v5.7-rc1
**Buggy since:** v5.7-rc1 (introduced by `ff7db0bf24db` which was merged in the same development cycle, both landed in v5.7-rc1 via the sched/core tip branch)

## Bug Description

The function `update_numa_stats()` in kernel/sched/fair.c iterates over all CPUs on a NUMA node to gather scheduling statistics used by the NUMA balancing migration logic. Commit `ff7db0bf24db` ("sched/numa: Prefer using an idle CPU as a migration target instead of comparing tasks") added a call to `numa_idle_core()` within this loop. `numa_idle_core()` in turn calls `test_idle_cores()`, which accesses the RCU-protected per-CPU variable `sd_llc_shared` via `rcu_dereference()`. However, `update_numa_stats()` was not holding the RCU read lock when making this call.

The `test_idle_cores()` function dereferences `per_cpu(sd_llc_shared, cpu)` using `rcu_dereference()`. This macro is an RCU-annotated pointer dereference that is only valid inside an RCU read-side critical section (i.e., between `rcu_read_lock()` and `rcu_read_unlock()`). Without the RCU read lock held, the scheduler domain structures being accessed could be freed concurrently during a domain rebuild, leading to a use-after-free or, at minimum, triggering the kernel's RCU lockdep warning machinery.

The call chain that triggers the bug is: `task_numa_migrate()` → `update_numa_stats()` → `numa_idle_core()` → `test_idle_cores()`. The `task_numa_migrate()` function is invoked from `task_numa_fault()` during NUMA page fault handling, which is called from the page fault handler (`do_page_fault()`). At this point, only `mm->mmap_sem` is held—no RCU read lock is active.

The bug was discovered by Qian Cai who observed a boot-time warning on kernel 5.6.0-rc3-next with CONFIG_PROVE_RCU enabled. The warning was deterministic and appeared early in the boot process when systemd (PID 1) triggered NUMA balancing.

## Root Cause

The root cause is a missing RCU read-side critical section in `update_numa_stats()`. The function was originally written without needing RCU protection because it only accessed per-runqueue data (`cpu_rq()`, `cpu_load()`, `cpu_util()`, etc.) which do not require RCU locking for safe access.

Commit `ff7db0bf24db` added the call to `numa_idle_core()` which, when CONFIG_SCHED_SMT is enabled, calls `test_idle_cores()`. The `test_idle_cores()` function is defined as:

```c
static inline bool test_idle_cores(int cpu, bool def)
{
    struct sched_domain_shared *sds;
    sds = rcu_dereference(per_cpu(sd_llc_shared, cpu));
    if (sds)
        return READ_ONCE(sds->has_idle_cores);
    return def;
}
```

The `rcu_dereference()` macro on `per_cpu(sd_llc_shared, cpu)` requires the caller to hold the RCU read lock. The `sd_llc_shared` pointer is part of the scheduler domain hierarchy which is rebuilt under RCU protection (via `call_rcu()` or `synchronize_rcu()`) during CPU hotplug or topology changes. Without RCU read lock protection, if a concurrent domain rebuild occurs, the `sds` pointer could point to freed memory after `rcu_dereference()` returns it but before `READ_ONCE(sds->has_idle_cores)` accesses the `has_idle_cores` field.

Additionally, `numa_idle_core()` also calls `is_core_idle()`, which iterates over `cpu_smt_mask(cpu)`. While `cpu_smt_mask()` itself may not strictly require RCU, the overall code path from `update_numa_stats()` iterating over `cpumask_of_node(nid)` and checking idle cores represents a scan of scheduler domain state that should be protected as a whole.

The specific lockdep check at fair.c line 5914 (in the buggy kernel version) fires because `rcu_dereference()` internally calls `rcu_dereference_check()` which validates that `rcu_read_lock_held()` returns true when CONFIG_PROVE_RCU is enabled. Since no RCU read lock was held, the check fails and emits the "suspicious RCU usage" warning.

## Consequence

The immediate observable consequence is a `WARNING: suspicious RCU usage` kernel warning printed during boot, triggered at `kernel/sched/fair.c:5914`. The warning includes the message "suspicious rcu_dereference_check() usage!" and a stack backtrace. This warning fires deterministically on NUMA systems with CONFIG_SCHED_SMT and CONFIG_PROVE_RCU enabled as soon as NUMA balancing is triggered by the first user process (systemd).

Beyond the warning, the missing RCU lock creates a theoretical race condition: if `update_numa_stats()` runs concurrently with a scheduler domain rebuild (e.g., during CPU hotplug), the `sd_llc_shared` pointer obtained by `test_idle_cores()` could reference freed memory. This could result in reading stale or corrupted `has_idle_cores` values, leading to incorrect idle core detection during NUMA migration decisions. In the worst case, if the freed memory is reused, this constitutes a use-after-free that could cause a kernel crash (NULL pointer dereference, general protection fault, or memory corruption).

In practice, since domain rebuilds are infrequent (typically only during boot, CPU hotplug, or cgroup topology changes) and the window for the race is small, the primary real-world impact is the spurious warning. However, on systems with aggressive CPU hotplug (e.g., power management-driven online/offline cycling), the race window becomes more significant.

## Fix Summary

The fix adds `rcu_read_lock()` before the `for_each_cpu()` loop in `update_numa_stats()` and `rcu_read_unlock()` after the loop completes. This wraps the entire CPU iteration—including the calls to `numa_idle_core()` → `test_idle_cores()` which require RCU protection—in a proper RCU read-side critical section.

The placement of the RCU lock is deliberately broad, covering the entire loop rather than just the `numa_idle_core()` call. As stated in the commit message, "while the locking could be fine-grained, it is more appropriate to acquire the RCU lock for the entire scan of the domain." This is correct because: (1) RCU read locks are extremely cheap (essentially a preempt_disable on non-PREEMPT_RCU kernels), so there is no meaningful performance cost; (2) wrapping the entire scan provides a clear, maintainable protection boundary for any future additions to the loop that might also access RCU-protected data; (3) the loop already accesses per-CPU runqueue data whose consistency benefits from a stable view of the scheduler domain hierarchy.

The fix was reviewed by Paul E. McKenney (RCU maintainer) and merged via the sched/core tip branch by Peter Zijlstra and Ingo Molnar. The two-line change is minimal and correct: it fully resolves the RCU lockdep warning and prevents the theoretical use-after-free race.

## Triggering Conditions

The following conditions are needed to trigger the bug:

- **Kernel version:** The kernel must contain commit `ff7db0bf24db` but not `0621df315402`. Both were merged in the v5.6-rc3 to v5.7-rc1 timeframe.
- **CONFIG_NUMA=y:** The kernel must have NUMA support enabled.
- **CONFIG_SCHED_SMT=y:** The `numa_idle_core()` function that calls `test_idle_cores()` is only compiled when SMT support is enabled. Without it, `numa_idle_core()` is a no-op stub that just returns `idle_core`.
- **CONFIG_PROVE_RCU=y:** Required to trigger the visible warning. Without this, the bug is silent (the RCU dereference still occurs without the lock, but no warning is emitted).
- **Multi-node NUMA topology:** The system must have at least 2 NUMA nodes to trigger NUMA balancing. The `update_numa_stats()` function is called from `task_numa_migrate()` which is part of the NUMA balancing path.
- **A running process that triggers NUMA faults:** The warning was observed with systemd (PID 1) during boot, triggered via `do_page_fault()` → NUMA balancing → `task_numa_migrate()` → `update_numa_stats()`.

The bug is highly deterministic on systems meeting the above criteria. The very first NUMA balancing operation after boot will trigger the warning. No special timing, race conditions, or workload patterns are required beyond normal NUMA page fault processing. The warning fires reliably every time `update_numa_stats()` is called with `find_idle=true` on a system with idle CPUs.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Kernel version too old (pre-v5.15):** The bug was introduced by commit `ff7db0bf24db` and fixed by commit `0621df315402dd7bc56f7272fae9778701289825`, both of which landed in the v5.7-rc1 merge window. kSTEP requires Linux v5.15 or newer. The buggy code path (without the `rcu_read_lock()`/`rcu_read_unlock()` pair) does not exist in any kernel version ≥ v5.15, as the fix was applied years before v5.15 was released.

2. **Nature of the bug:** Even if the kernel version were supported, this bug is an RCU locking correctness issue rather than a functional scheduling bug. The primary manifestation is a lockdep warning (`WARNING: suspicious RCU usage`), which is a static analysis / runtime verification check, not a scheduling misbehavior. The actual race condition (use-after-free of `sd_llc_shared`) requires a concurrent scheduler domain rebuild during `update_numa_stats()` execution, which would need real CPU hotplug events occurring with precise timing.

3. **NUMA balancing dependency:** The buggy code path is in the NUMA balancing subsystem (`task_numa_migrate()` → `update_numa_stats()`), which is triggered by NUMA page faults. kSTEP cannot simulate NUMA memory access latencies or NUMA page faults. While kSTEP can set up NUMA topology structure, it cannot trigger the actual NUMA fault handling path that invokes `task_numa_migrate()`.

4. **What would be needed:** To reproduce this with kSTEP on a supported kernel version, one would need: (a) a kernel version between v5.6-rc3 and v5.7-rc1 where the bug exists; (b) a way to trigger `task_numa_migrate()` from a kSTEP driver, which would require calling NUMA-internal functions directly; (c) CONFIG_PROVE_RCU to make the warning visible; and (d) a concurrent domain rebuild to trigger the actual race rather than just the warning.

5. **Alternative reproduction methods:** Outside of kSTEP, this bug can be trivially reproduced by booting a kernel containing commit `ff7db0bf24db` but not `0621df315402` on any NUMA system with CONFIG_PROVE_RCU=y and CONFIG_SCHED_SMT=y. The warning appears automatically during boot without any special workload.
