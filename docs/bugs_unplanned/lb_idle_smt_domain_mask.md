# LB: select_idle_smt() Selects CPUs Outside Scheduling Domain

**Commit:** `df3cb4ea1fb63ff326488efd671ba3c39034255e`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v5.10-rc1
**Buggy since:** v4.9-rc1 (introduced by commit `10e2f1acd010` "sched/core: Rewrite and improve select_idle_siblings()")

## Bug Description

When a task wakes up, the CFS scheduler invokes `select_idle_sibling()` to find an idle CPU for placement. One of the helpers called in this path is `select_idle_smt()`, which scans the SMT (Simultaneous Multi-Threading / Hyperthreading) siblings of a target CPU looking for an idle sibling to run the waking task. The bug is that `select_idle_smt()` only checks whether the candidate CPU is in the task's CPU affinity mask (`p->cpus_ptr`), but does NOT check whether the candidate CPU belongs to the scheduling domain (`sched_domain_span(sd)`). This means it can select CPUs that were explicitly isolated from the scheduling domain via the `isolcpus=domain` boot parameter.

The `isolcpus=domain` kernel boot parameter is used in production environments to reserve specific CPUs for dedicated workloads such as real-time threads, DPDK packet processing, or latency-sensitive applications. When a CPU is isolated via `isolcpus=domain`, it is removed from all scheduling domains, meaning the scheduler should never place general-purpose tasks on it. However, because `select_idle_smt()` iterates over `cpu_smt_mask(target)` (which is based on hardware topology and always includes all physical SMT siblings regardless of scheduling domain configuration), it can return an isolated CPU as a valid idle target.

The bug was introduced in v4.9-rc1 by commit `10e2f1acd010` ("sched/core: Rewrite and improve select_idle_siblings()") which added the `select_idle_smt()` function. The function was written to check only `p->cpus_ptr` (task affinity) and `available_idle_cpu()` / `sched_idle_cpu()`, but never `sched_domain_span(sd)`. The Alibaba team discovered the bug in production when they observed tasks being migrated to isolated CPUs on a 31-CPU hyperthreaded machine with `isolcpus=domain,2-31`.

Note that `select_idle_core()`, the sibling function that searches for an entirely idle core, correctly checks the scheduling domain mask via `cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr)`. The omission in `select_idle_smt()` was an oversight in the original implementation.

## Root Cause

The function `select_idle_smt()` in `kernel/sched/fair.c` is called from `select_idle_sibling()` as a fallback after `select_idle_core()` and `select_idle_cpu()` both fail to find an idle CPU. Its purpose is to scan the SMT siblings of the target CPU for an idle or sched-idle CPU. The buggy code is:

```c
static int select_idle_smt(struct task_struct *p, int target)
{
    int cpu;

    if (!static_branch_likely(&sched_smt_present))
        return -1;

    for_each_cpu(cpu, cpu_smt_mask(target)) {
        if (!cpumask_test_cpu(cpu, p->cpus_ptr))
            continue;
        if (available_idle_cpu(cpu) || sched_idle_cpu(cpu))
            return cpu;
    }

    return -1;
}
```

The loop iterates over `cpu_smt_mask(target)`, which is derived from the hardware topology and includes all physical SMT siblings of `target`, regardless of any scheduling domain configuration. The only filter applied is `cpumask_test_cpu(cpu, p->cpus_ptr)`, which checks the task's CPU affinity mask. There is no check against `sched_domain_span(sd)`.

When `isolcpus=domain,X` is specified on the kernel boot command line, CPU X is removed from all scheduling domains. The scheduling domain span (`sched_domain_span(sd)`) for any non-isolated CPU will NOT include CPU X. However, `cpu_smt_mask()` is based on hardware topology and always includes CPU X if it is a physical SMT sibling. Similarly, `p->cpus_ptr` may include CPU X if the task has a permissive affinity (the default is all CPUs, and cpusets do not automatically exclude isolated CPUs from a task's affinity).

The call site in `select_idle_sibling()` that invokes `select_idle_smt()` is:

```c
sd = rcu_dereference(per_cpu(sd_llc, target));
if (!sd)
    return target;

i = select_idle_core(p, sd, target);
if ((unsigned)i < nr_cpumask_bits)
    return i;

i = select_idle_cpu(p, sd, target);
if ((unsigned)i < nr_cpumask_bits)
    return i;

i = select_idle_smt(p, target);  /* BUG: sd not passed */
if ((unsigned)i < nr_cpumask_bits)
    return i;
```

Notice that `select_idle_core()` and `select_idle_cpu()` both receive the `sd` parameter and correctly filter by `sched_domain_span(sd)`, but `select_idle_smt()` does not receive `sd` at all. This asymmetry is the root cause: the function simply has no way to know which CPUs are in the scheduling domain.

Consider a concrete example from the reporter's machine: a 32-CPU system (16 cores × 2 threads) booted with `isolcpus=domain,2-31`. The SMT pairs are {0,16}, {1,17}, {2,18}, ..., {15,31}. CPUs 0 and 1 are in the scheduling domain; CPUs 2-31 are isolated. When a task last ran on CPU 1 (prev=1) and `select_idle_smt(p, prev=1)` is called, the function iterates over `cpu_smt_mask(1)` = {1, 17}. CPU 17 is an SMT sibling of CPU 1 in hardware, but it is isolated from the scheduling domain. Since CPU 17 is idle (isolated CPUs have no tasks) and in the task's affinity, `select_idle_smt()` returns 17, causing the task to be placed on an isolated CPU.

## Consequence

The observable impact is that tasks are incorrectly placed on CPUs that were explicitly isolated from the scheduling domain via `isolcpus=domain`. This violates the system administrator's intent to keep those CPUs free from general task scheduling. The consequences are:

1. **Isolation violation**: Tasks that should never run on isolated CPUs end up there, defeating the purpose of CPU isolation. In production environments, `isolcpus=domain` is a critical tool for reserving CPUs for latency-sensitive workloads (real-time threads, DPDK networking, high-frequency trading). Violations can have significant operational impact.

2. **Performance interference**: When an unexpected task lands on an isolated CPU that is dedicated to a specific workload, it causes SMT resource contention (shared execution pipelines, L1 cache, TLB). This leads to unpredictable latency spikes for the dedicated workload, which may have strict real-time requirements.

3. **Silent misbehavior**: There is no crash, kernel panic, or warning. The bug silently places tasks on wrong CPUs. The only way to detect it is by monitoring `task_cpu()` or observing unexpected load on isolated CPUs, making it difficult to diagnose in production.

The Alibaba production team confirmed this on their 31-CPU hyperthreaded machine with `isolcpus=domain,2-31`. They observed that tasks placed in a cpuset (via `cgcreate -g cpu:test; cgexec -g cpu:test "test_threads"`) would occasionally be migrated to isolated CPUs 16 and 17 (the SMT siblings of non-isolated CPUs 0 and 1). The bug is probabilistic in that it depends on `select_idle_smt()` being reached (requires `select_idle_core()` and `select_idle_cpu()` to both fail), but once that code path is hit, the misbehavior is deterministic: isolated idle CPUs will always be selected because they are always idle.

## Fix Summary

The fix in commit `df3cb4ea1fb63ff326488efd671ba3c39034255e` adds the missing scheduling domain check to `select_idle_smt()`. The changes are:

1. **Function signature change**: `select_idle_smt()` gains a `struct sched_domain *sd` parameter in both the CONFIG_SCHED_SMT and non-SMT stub versions:
   ```c
   -static int select_idle_smt(struct task_struct *p, int target)
   +static int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
   ```

2. **Domain check added to the loop body**: Inside the SMT sibling iteration, a check against `sched_domain_span(sd)` is added alongside the existing `p->cpus_ptr` check:
   ```c
   for_each_cpu(cpu, cpu_smt_mask(target)) {
   -    if (!cpumask_test_cpu(cpu, p->cpus_ptr))
   +    if (!cpumask_test_cpu(cpu, p->cpus_ptr) ||
   +        !cpumask_test_cpu(cpu, sched_domain_span(sd)))
           continue;
       if (available_idle_cpu(cpu) || sched_idle_cpu(cpu))
           return cpu;
   }
   ```
   Now a candidate SMT sibling is skipped if it is NOT in the task's affinity mask OR if it is NOT in the scheduling domain span. This ensures isolated CPUs are filtered out.

3. **Call site updated**: The call in `select_idle_sibling()` passes the `sd` that was already looked up earlier:
   ```c
   -i = select_idle_smt(p, target);
   +i = select_idle_smt(p, sd, target);
   ```

The fix is correct because `sd` is the LLC scheduling domain for `target`, which is guaranteed to be valid at this point in the code (there is an early return if `sd` is NULL). By checking `sched_domain_span(sd)`, the function now correctly respects CPU isolation boundaries. This brings `select_idle_smt()` into alignment with `select_idle_core()`, which already performs the same domain check via `cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr)`. The non-SMT stub also receives the `sd` parameter for API consistency, but continues to always return -1 since there are no SMT siblings to scan.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger this bug:

1. **SMT (Hyperthreading) must be enabled**: The system must have SMT-capable CPUs with at least 2 hardware threads per core, and `static_branch_likely(&sched_smt_present)` must be true. On x86, this means Intel Hyperthreading or AMD SMT must be enabled in BIOS and not disabled via kernel parameters.

2. **`isolcpus=domain` must isolate an SMT sibling of a non-isolated CPU**: The kernel must be booted with `isolcpus=domain,...` where the isolated CPU set includes at least one CPU that is an SMT sibling of a non-isolated CPU. For example, on a system where CPUs {0,16} form an SMT pair, `isolcpus=domain,16` creates the vulnerable configuration: CPU 0 is in the scheduling domain, CPU 16 is not, but both are hardware SMT siblings.

3. **The waking task's affinity must include the isolated CPU**: The task's `p->cpus_ptr` must include the isolated CPU. This is the default case (all CPUs) unless the task has been explicitly restricted. Tasks placed in cpusets inherit the cpuset's mask, which may or may not include isolated CPUs depending on configuration.

4. **`select_idle_smt()` must be reached in the call chain**: This requires both `select_idle_core()` and `select_idle_cpu()` to fail to find an idle CPU. `select_idle_core()` fails when `test_idle_cores(target)` returns false (no idle cores detected). `select_idle_cpu()` fails when there are no idle CPUs in the domain within the scan budget. This happens under moderate to heavy load when most domain CPUs are busy.

5. **The isolated SMT sibling must be idle**: The isolated CPU must satisfy `available_idle_cpu(cpu)` or `sched_idle_cpu(cpu)`. Since isolated CPUs are excluded from the scheduling domain, they typically have no runnable tasks and are always idle. This condition is almost always satisfied, making the bug easy to trigger once the other conditions are met.

There is no race condition or timing requirement. Once the code path reaches `select_idle_smt()` with a target CPU that has an idle isolated SMT sibling, the function deterministically returns the isolated CPU. The bug is probabilistic only in that it requires `select_idle_core()` and `select_idle_cpu()` to fail first, which depends on system load.

## Reproduce Strategy (kSTEP)

### Why This Bug Cannot Be Reproduced With kSTEP

This bug CANNOT be reproduced with kSTEP because the kernel version is too old.

**1. Kernel version incompatibility (primary reason):**
The fix commit `df3cb4ea1fb63ff326488efd671ba3c39034255e` was merged into v5.10-rc1 (September 2020). This means the bug exists only in kernels from v4.9-rc1 (when `select_idle_smt()` was introduced by commit `10e2f1acd010`) through v5.9.x (the last release before v5.10-rc1). kSTEP supports Linux v5.15 and newer only. Since the fix was applied before v5.15, there is no kSTEP-compatible kernel version that contains this bug. Checking out `df3cb4ea~1` would produce a v5.9-era kernel that kSTEP cannot build or run.

**2. What would be needed to reproduce this bug:**
If kSTEP supported pre-v5.15 kernels, reproduction would require:
- **SMT topology in QEMU**: Boot QEMU with `-smp 4,cores=2,threads=2,sockets=1` to create real SMT pairs ({0,2} and {1,3}).
- **`isolcpus=domain` boot parameter**: Boot the kernel with `isolcpus=domain,2-3` to isolate the SMT siblings of CPUs 0 and 1 from the scheduling domain.
- **Task wakeup under load**: Create CFS tasks with full affinity, keep domain CPUs busy so `select_idle_core()` and `select_idle_cpu()` fail, then wake a task and observe whether it lands on an isolated CPU.

**3. Alternative reproduction methods:**
This bug could be reproduced outside kSTEP using:
- A v5.9 or earlier kernel running on real hardware with Hyperthreading enabled, or in a VM with explicit SMT topology.
- Boot with `isolcpus=domain,<sibling_cpus>` and run a threaded workload in a cpuset.
- Monitor task placement with `taskset`, `/proc/<pid>/status` (Cpus_allowed), or `perf sched` to detect tasks landing on isolated CPUs.

**4. Note on the later recurrence:**
This exact class of bug recurred later in commit `3e6efe87cd5c` ("sched/fair: Remove redundant check in select_idle_smt()") which removed the domain check as a "cleanup." That regression was fixed by `8aeaffef8c6e` (tracked separately in `kmod/drivers_planned/lb_select_idle_smt_isolcpus.md`). The later instance IS reproducible with kSTEP since it exists in v6.1+ kernels.
