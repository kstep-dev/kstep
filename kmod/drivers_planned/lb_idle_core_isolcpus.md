# LB: select_idle_core() Ignores Scheduling Domain with isolcpus

**Commit:** `23d04d8c6b8ec339057264659b7834027f3e6a63`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.9-rc1
**Buggy since:** v5.12-rc1 (commit 9fe1f127b913 "sched/fair: Merge select_idle_core/cpu()")

## Bug Description

The `select_idle_core()` function in the CFS wake-up path fails to respect the scheduling domain boundary when selecting an idle CPU as a fallback. Specifically, it checks `p->cpus_ptr` (the task's full affinity mask) instead of the `cpus` mask (which is the intersection of `p->cpus_ptr` and `sched_domain_span(sd)`). This means that when CPUs are isolated from the scheduling domain via the `isolcpus` kernel command line option, `select_idle_core()` can still select those isolated CPUs as candidates for task placement.

The `isolcpus=domain,<cpulist>` boot parameter removes specified CPUs from the default scheduling domain while keeping them online. Tasks with default affinity still have these isolated CPUs in their `p->cpus_ptr`, but the scheduling domain's span (`sched_domain_span(sd)`) does not include them. The caller `select_idle_cpu()` correctly computes `cpus = sched_domain_span(sd) & p->cpus_ptr` to restrict the search space, but `select_idle_core()` bypasses this restriction by checking `p->cpus_ptr` directly.

This bug was introduced in commit 9fe1f127b913 ("sched/fair: Merge select_idle_core/cpu()") in v5.12-rc1, which restructured `select_idle_core()` and `select_idle_cpu()` to share a single iteration loop. In the old code, `select_idle_core()` maintained its own `cpus` mask computed as `cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr)`. After the merge, the `cpus` mask is computed by `select_idle_cpu()` and passed to `select_idle_core()` as a parameter, but the checks inside `select_idle_core()` were written using `p->cpus_ptr` instead of the passed `cpus` mask.

This was part of a 2-patch series. The companion patch fixed the same class of bug in `select_idle_smt()`, which also failed to check the scheduling domain when scanning SMT siblings.

## Root Cause

The root cause is two incorrect uses of `p->cpus_ptr` in `select_idle_core()` where `cpus` (the domain-restricted mask) should be used.

In the buggy `select_idle_core()` function:

```c
static int select_idle_core(struct task_struct *p, int core, struct cpumask *cpus, int *idle_cpu)
{
    bool idle = true;
    int cpu;

    for_each_cpu(cpu, cpu_smt_mask(core)) {
        if (!available_idle_cpu(cpu)) {
            idle = false;
            if (*idle_cpu == -1) {
                if (sched_idle_cpu(cpu) && cpumask_test_cpu(cpu, p->cpus_ptr)) {  // BUG
                    *idle_cpu = cpu;
                    break;
                }
                continue;
            }
            break;
        }
        if (*idle_cpu == -1 && cpumask_test_cpu(cpu, p->cpus_ptr))  // BUG
            *idle_cpu = cpu;
    }

    if (idle)
        return core;

    cpumask_andnot(cpus, cpus, cpu_smt_mask(core));
    return -1;
}
```

There are two places where `p->cpus_ptr` is incorrectly used instead of `cpus`:

1. **Line with `sched_idle_cpu(cpu)` fallback** (first bug site): When a CPU is not fully idle but is in the `SCHED_IDLE` policy, the function checks if the CPU is in `p->cpus_ptr`. If the CPU is domain-isolated, it is in `p->cpus_ptr` (the task has default affinity) but NOT in `cpus` (which excludes domain-isolated CPUs). The buggy code sets `*idle_cpu = cpu` for the isolated CPU.

2. **Line with `available_idle_cpu(cpu)` success** (second bug site): When a CPU is fully idle, the function checks if the CPU is in `p->cpus_ptr` before recording it as `*idle_cpu`. Again, a domain-isolated idle CPU would pass this check incorrectly.

The `cpus` parameter is computed by the caller `select_idle_cpu()` as:

```c
cpumask_and(cpus, sched_domain_span(sd), p->cpus_ptr);
```

This correctly restricts the mask to CPUs that are both in the task's affinity AND in the scheduling domain. However, `select_idle_core()` iterates `for_each_cpu(cpu, cpu_smt_mask(core))`, which covers ALL SMT siblings of the core — including those outside the scheduling domain. The `cpumask_test_cpu(cpu, p->cpus_ptr)` check is insufficient because it does not account for the domain restriction.

The outer loop in `select_idle_cpu()` iterates `for_each_cpu_wrap(cpu, cpus, target + 1)`, so the `core` parameter passed to `select_idle_core()` is always within `cpus`. But the SMT siblings of that core may include CPUs outside `cpus`. That is where the bug manifests.

## Consequence

The observable impact is that tasks can be incorrectly placed on domain-isolated CPUs during wakeup. This defeats the purpose of the `isolcpus` kernel command line option, which is specifically designed to keep general workload tasks away from isolated CPUs.

When `isolcpus=domain,<cpulist>` is used, the administrator's intent is to reserve those CPUs for specific latency-sensitive or real-time workloads (often pinned explicitly via `taskset` or `sched_setaffinity`). If the scheduler's idle CPU selection bypasses the domain boundary and places unrelated CFS tasks on these isolated CPUs, it causes:

1. **Isolation violation**: The isolated CPUs experience unexpected interference from general-purpose tasks, potentially causing latency spikes for the workloads that were supposed to have exclusive access.
2. **Performance degradation**: Tasks placed on domain-isolated CPUs won't benefit from load balancing (since the domain doesn't cover those CPUs), potentially leading to suboptimal CPU utilization — tasks may pile up on isolated CPUs without being migrated away.
3. **Unpredictable behavior**: The `*idle_cpu` fallback is used when no fully idle core is found. The returned CPU is used directly as the wakeup target. On systems with heavy load (where fully idle cores are rare), this fallback path is exercised frequently, making the bug more impactful.

This bug is particularly problematic in production environments using `isolcpus` for latency-critical workloads (e.g., network packet processing, financial trading systems, real-time audio/video processing), where stray tasks on isolated CPUs can cause measurable performance regressions.

## Fix Summary

The fix is a minimal two-line change that replaces `p->cpus_ptr` with `cpus` in both buggy locations within `select_idle_core()`:

```c
-               if (sched_idle_cpu(cpu) && cpumask_test_cpu(cpu, p->cpus_ptr)) {
+               if (sched_idle_cpu(cpu) && cpumask_test_cpu(cpu, cpus)) {
```

```c
-       if (*idle_cpu == -1 && cpumask_test_cpu(cpu, p->cpus_ptr))
+       if (*idle_cpu == -1 && cpumask_test_cpu(cpu, cpus))
```

By checking `cpus` instead of `p->cpus_ptr`, the function now correctly restricts idle CPU selection to CPUs that are within both the task's affinity mask and the current scheduling domain's span. A domain-isolated CPU (one that is in `p->cpus_ptr` but not in `sched_domain_span(sd)`) will fail the `cpumask_test_cpu(cpu, cpus)` check and will not be recorded as `*idle_cpu`.

This fix is correct and complete because `cpus` already encodes the domain restriction (it is the intersection of `sched_domain_span(sd)` and `p->cpus_ptr`), and it is the same mask that the outer loop in `select_idle_cpu()` uses for its iteration. The fix ensures that the inner loop in `select_idle_core()` is consistent with the outer loop's search space.

## Triggering Conditions

The following conditions are all necessary to trigger the bug:

1. **CONFIG_SCHED_SMT=y**: The `select_idle_core()` function only exists when SMT support is compiled in. Without it, the function is a stub that returns `__select_idle_cpu(core)`.

2. **SMT topology**: The system must have at least one core with multiple SMT threads (hyperthreading). This is required for `for_each_cpu(cpu, cpu_smt_mask(core))` to iterate more than one CPU.

3. **Domain-isolated CPUs via `isolcpus=domain,<cpulist>`**: At least one CPU must be removed from the scheduling domain using `isolcpus`. Specifically, an SMT sibling of a non-isolated CPU must be domain-isolated. This creates the mismatch between `p->cpus_ptr` (which includes the isolated CPU) and `sched_domain_span(sd)` (which excludes it).

4. **`has_idle_cores` is true**: The `select_idle_core()` path in `select_idle_cpu()` is only taken when `test_idle_cores(target)` returns true, meaning `sd_llc_shared->has_idle_cores` is set. This flag is set by `__update_idle_core()` when all SMT siblings in a core become idle.

5. **Core is NOT fully idle**: The `*idle_cpu` fallback (where the bug manifests) is only used when at least one SMT sibling is not idle (`idle = false`). If all siblings are idle, the function returns the core directly (which is always in the domain). So the core must have a mix of idle and busy CPUs.

6. **No prior `*idle_cpu` found**: The `*idle_cpu` variable starts at -1 and is only set once. If a valid (in-domain) idle CPU is found first in the SMT iteration order, the domain-isolated CPU won't override it. The iteration order is `for_each_cpu(cpu, cpu_smt_mask(core))`, which typically iterates in CPU number order.

7. **Task wakeup on a non-isolated CPU**: The wakeup must be processed on a CPU that has a valid scheduling domain (i.e., a non-domain-isolated CPU). Domain-isolated CPUs have no scheduling domain (`sd_llc` is NULL), so `select_idle_cpu()` returns -1 immediately.

A concrete scenario: On a 4-CPU system with SMT (core 0 = {CPU 0, CPU 1}, core 1 = {CPU 2, CPU 3}), boot with `isolcpus=domain,1,3`. CPU 0's scheduling domain spans only {0, 2}. A CFS task with default affinity (`p->cpus_ptr = {0,1,2,3}`) wakes up on CPU 0. The scheduler calls `select_idle_core()` for core 0 ({CPU 0, CPU 1}). CPU 0 is busy (running the waker), CPU 1 is idle and domain-isolated. The buggy code checks `cpumask_test_cpu(1, p->cpus_ptr)` → true, and sets `*idle_cpu = 1`. The task is placed on CPU 1, violating the isolation.

The bug is deterministic given the right topology and isolation setup; there is no race condition or timing dependency. It triggers on every wakeup that goes through this code path under the described conditions.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP with a minor framework extension: kSTEP's boot parameters must be modified to include `domain` in the `isolcpus` flags for specific CPUs, creating the scheduling-domain vs. affinity mismatch that triggers the bug.

### Required kSTEP Modification

Currently, kSTEP boots with `isolcpus=nohz,managed_irq,{isol_cpus}`, which isolates workload CPUs from tick and IRQ handling but does NOT remove them from scheduling domains. To reproduce this bug, the boot args must include `domain` isolation. The modification is:

In `run.py`, change the `isolcpus` boot argument to include the `domain` flag:
```python
f"isolcpus=domain,nohz,managed_irq,{isol_cpus}",
```

This is a one-word change in a single line. Alternatively, add a driver-level parameter that optionally includes `domain` in the isolcpus flags.

**Impact on kSTEP**: With domain isolation, workload CPUs are removed from the default scheduling domain. CPU 0's scheduling domain only covers {0}. Tasks placed on workload CPUs via `kstep_task_pin()` will still run there (affinity overrides domain for placement), but they won't be load-balanced. For this specific bug reproduction, we explicitly control task placement, so the lack of load balancing is not an issue.

### Driver Configuration

- **CPUs**: 4 CPUs with SMT topology
  - Core 0: CPU 0 (driver), CPU 1 (workload, domain-isolated)
  - Core 1: CPU 2 (workload, domain-isolated), CPU 3 (workload, domain-isolated)
- **Topology setup**:
  ```c
  kstep_topo_init();
  const char *smt[] = {"0-1", "0-1", "2-3", "2-3"};
  kstep_topo_set_smt(smt, 4);
  kstep_topo_apply();
  ```
- **Boot**: `isolcpus=domain,nohz,managed_irq,1-3` (all workload CPUs domain-isolated)
- **`num_cpus`**: 4

### Step-by-Step Reproduction

1. **Setup topology**: Configure 4 CPUs with SMT as above. After `kstep_topo_apply()`, CPU 0's LLC scheduling domain spans only {0} (CPUs 1-3 are domain-isolated).

2. **Ensure `has_idle_cores` is true**: Since CPUs 1-3 are idle (no tasks running on them), `__update_idle_core()` will mark cores as having idle siblings. We need the `sd_llc_shared->has_idle_cores` flag for CPU 0's domain. Since CPU 0's domain is {0} and core 0 includes {0, 1}, the core is not fully idle if CPU 0 is running. We need to trigger the idle core flag by ticking a few times with CPU 0 momentarily idle.

   Alternatively, use `KSYM_IMPORT()` to access and manually set `sd_llc_shared->has_idle_cores` for CPU 0. This forces the `select_idle_core()` path.

3. **Create a CFS task**: `struct task_struct *p = kstep_task_create();` — creates a task with default affinity (`p->cpus_ptr` includes all online CPUs {0,1,2,3}).

4. **Block and wake the task**: The task starts on a workload CPU. Block it with `kstep_task_block(p)`, then wake it with `kstep_task_wakeup(p)`. The wakeup is processed on CPU 0 (the driver CPU), which calls `select_task_rq_fair()` → `select_idle_sibling()` → `select_idle_cpu()` → `select_idle_core()`.

5. **Check task placement**: After wakeup, read `task_cpu(p)` to determine which CPU the task was placed on.

### Detection Criteria

- **Buggy kernel**: `task_cpu(p)` may be 1 (a domain-isolated SMT sibling of CPU 0). The `select_idle_core()` function scans core 0's SMT siblings {0, 1}. CPU 0 is busy (running driver), CPU 1 is idle. The buggy check `cpumask_test_cpu(1, p->cpus_ptr)` passes, so `*idle_cpu = 1`. This is returned as the wakeup target. Report with `kstep_fail("task placed on domain-isolated CPU %d", task_cpu(p))`.

- **Fixed kernel**: `task_cpu(p)` should NOT be 1 (or any domain-isolated CPU). The fixed check `cpumask_test_cpu(1, cpus)` fails because CPU 1 is not in `cpus = sched_domain_span(sd) & p->cpus_ptr = {0}`. The `*idle_cpu` stays -1, and the scheduler falls back to other heuristics. Report with `kstep_pass("task correctly placed on CPU %d", task_cpu(p))`.

### Callbacks

Use `on_tick_begin` to log `task_cpu(p)` after each tick following the wakeup:
```c
static void on_tick_begin(void) {
    kstep_output_curr_task();
}
```

### Internal State Access

Use `KSYM_IMPORT()` to access:
- `sd_llc_shared` — to check/force `has_idle_cores`
- `per_cpu(sd_llc, cpu)` — to verify scheduling domain span
- `cpu_rq(cpu)->curr` — to check which task is running on each CPU

### Expected Driver Structure

```c
#include "driver.h"
#include "internal.h"

static struct task_struct *task;

static void setup(void) {
    kstep_topo_init();
    const char *smt[] = {"0-1", "0-1", "2-3", "2-3"};
    kstep_topo_set_smt(smt, 4);
    kstep_topo_apply();

    task = kstep_task_create();
}

static void run(void) {
    // Pin task initially to CPU 2 (a workload CPU)
    kstep_task_pin(task, 2, 2);
    kstep_task_wakeup(task);
    kstep_tick_repeat(10);

    // Block the task, then wake it (wakeup runs on CPU 0)
    kstep_task_block(task);
    kstep_tick();

    // Now wake it with wide affinity: cpus_ptr = {0,1,2,3}
    kstep_task_pin(task, 0, 3);
    kstep_task_wakeup(task);
    kstep_tick();

    int cpu = task_cpu(task);
    // CPU 1 and 3 are domain-isolated; task should not be placed there
    if (cpu == 1 || cpu == 3) {
        kstep_fail("task placed on domain-isolated CPU %d", cpu);
    } else {
        kstep_pass("task correctly placed on CPU %d", cpu);
    }
}

KSTEP_DRIVER_DEFINE {
    .name = "lb_idle_core_isolcpus",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
};
```

### Alternative Approach Without Boot Arg Changes

If modifying the `isolcpus` boot parameter is undesirable, an alternative approach is to use kSTEP's `KSYM_IMPORT()` to directly access and manipulate the scheduling domain structures at runtime. Specifically, one could narrow a scheduling domain's span by clearing bits in the domain's `span` cpumask after `kstep_topo_apply()`. This would simulate the effect of `isolcpus=domain` without changing the boot parameters. However, this approach is fragile and may trigger assertions in the scheduler code that assumes domain consistency.

A more robust alternative would be to add a kSTEP API function `kstep_isolcpus_domain(mask)` that calls the kernel's internal housekeeping/partition machinery to properly domain-isolate CPUs at runtime. This would require hooking into `housekeeping_setup()` or rebuilding scheduling domains after modifying the housekeeping masks.
