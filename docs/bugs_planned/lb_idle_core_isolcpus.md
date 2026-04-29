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

This bug is reproduced entirely through public kernel interfaces—the `isolcpus=domain` boot parameter and kSTEP's SMT topology API—without writing to any internal scheduler state. The core idea is to create an SMT topology where one sibling of the driver core is domain-isolated, then trigger a CFS task wakeup on CPU 0 so that `select_idle_core()` scans the full `cpu_smt_mask()` (which includes the isolated sibling) and incorrectly selects it via the buggy `p->cpus_ptr` check. The `has_idle_cores` flag, which gates entry to the `select_idle_core()` path, is set naturally by the kernel's own `__update_idle_core()` when CPU 0 transitions to idle while its SMT sibling (CPU 1) is already idle.

### Required kSTEP Modification

Currently, kSTEP boots with `isolcpus=nohz,managed_irq,{isol_cpus}`, which isolates workload CPUs from tick and IRQ handling but does NOT remove them from scheduling domains. To reproduce this bug, the boot args must include `domain` isolation. The modification is a one-word change in `run.py`:

```python
f"isolcpus=domain,nohz,managed_irq,{isol_cpus}",
```

With `domain` isolation, workload CPUs are removed from the default scheduling domain hierarchy. CPU 0's LLC scheduling domain (`sd_llc`) spans only {0}. Crucially, `cpu_smt_mask(0)` still returns {0, 1} because SMT topology is physical, not domain-based. This creates the exact mismatch the bug exploits: `select_idle_core()` iterates all SMT siblings via `cpu_smt_mask()` but should restrict candidate selection to the domain-restricted `cpus` mask. Tasks placed on workload CPUs via `kstep_task_pin()` still run there (affinity overrides domain for placement), but they are invisible to load balancing, which is acceptable since this driver controls all placement explicitly.

### Driver Configuration

- **CPUs**: 4 CPUs with SMT-2 topology (2 physical cores, 2 threads each)
  - Core 0: CPU 0 (driver, in scheduling domain), CPU 1 (workload, domain-isolated)
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

After `kstep_topo_apply()`, the kernel rebuilds scheduling domains. With `isolcpus=domain,...,1-3`, CPU 0's `sd_llc` domain spans {0} only. CPUs 1–3 have no scheduling domain at all (`per_cpu(sd_llc, 1)` is NULL). However, `cpu_smt_mask(0)` returns {0, 1} because the SMT sibling relationship is topology-derived, not domain-derived. This is the key asymmetry: `select_idle_core()` iterates physical SMT siblings but should restrict its choices to the scheduling domain.

### Naturally Setting `has_idle_cores`

The `select_idle_core()` path in `select_idle_cpu()` is gated by `test_idle_cores(target)`, which reads `sd_llc_shared->has_idle_cores`. This flag is set by `__update_idle_core()`, which the kernel calls when a CPU enters the idle path (via `pick_next_task_idle()` → `__update_idle_core()`). The function checks whether ALL SMT siblings of the core are idle—importantly, it uses `cpu_smt_mask()` which includes domain-isolated siblings.

To trigger this naturally, call `kstep_sleep()` after blocking all tasks on CPU 0. This yields the driver thread, allowing CPU 0 to enter the idle path. At that point, `__update_idle_core()` runs for core 0: it iterates `cpu_smt_mask(0) = {0, 1}` and finds CPU 1 is idle (domain-isolated, no tasks). Since both siblings are idle, it calls `set_idle_cores(0, 1)`, setting `has_idle_cores = 1` on CPU 0's `sd_llc_shared`. When the driver wakes up from `kstep_sleep()`, the flag persists—the driver's own wakeup goes through `try_to_wake_up()` → `select_idle_sibling()`, but since the driver is pinned to CPU 0 and CPU 0 is idle at that moment, `select_idle_sibling()` returns CPU 0 immediately via the `available_idle_cpu(target)` fast path, never reaching `select_idle_cpu()` and thus never clearing `has_idle_cores`.

### Ensuring the Wakeup Reaches `select_idle_core()`

A critical subtlety: the test task's `prev_cpu` (the CPU it last ran on) must be CPU 0 for the wakeup to traverse the `select_idle_core()` code path. If `prev_cpu` is a domain-isolated CPU (e.g., CPU 2), then `select_idle_sibling()` looks up `sd_llc` for the target CPU derived from `prev_cpu`. Since domain-isolated CPUs have `sd_llc = NULL`, the function returns immediately without any idle scanning. To avoid this, the task is first pinned to CPU 0 and allowed to run there, establishing `prev_cpu = 0`.

Additionally, `select_idle_sibling()` has several early-return paths that must NOT short-circuit:
1. **Target idle check**: `available_idle_cpu(target=0)` — CPU 0 is busy (running the driver's `kstep_task_wakeup()` call), so this fails. ✓
2. **Prev idle check**: `prev == target` (both are 0), so this is skipped. ✓
3. **Recent-used CPU**: `p->recent_used_cpu` is 0 (same as target), so `recent_used_cpu != target` fails, skipping this path. ✓
4. **`select_idle_smt()`**: Scans `cpu_smt_mask(0) = {0, 1}`. CPU 1 is checked against `sched_domain_span(sd) = {0}` — the companion patch (already applied in the buggy kernel version targeted by this commit) correctly rejects CPU 1 here. Returns -1. ✓
5. **`select_idle_cpu()`**: Entered with `has_idle_core = true`. This is where the bug triggers.

### Step-by-Step Reproduction

1. **Setup topology**: Configure 4 CPUs with SMT-2 as described above. After `kstep_topo_apply()`, CPU 0's LLC domain spans {0}, while `cpu_smt_mask(0)` covers {0, 1}.

2. **Create the test task**: `struct task_struct *task = kstep_task_create();` — the task starts in a ready/paused state with default affinity (`p->cpus_ptr = {0, 1, 2, 3}`).

3. **Establish `prev_cpu = 0`**: Pin the task to CPU 0 with `kstep_task_pin(task, 0, 0)`, then wake it with `kstep_task_wakeup(task)`. Advance several ticks with `kstep_tick_repeat(5)` so the task actually runs on CPU 0 and the scheduler records `task_cpu(task) = 0`.

4. **Block the task**: Call `kstep_task_block(task)` followed by `kstep_tick()`. The task enters `TASK_INTERRUPTIBLE` sleep. CPU 0 now only has the driver thread. The task's `prev_cpu` is frozen at 0.

5. **Naturally set `has_idle_cores`**: Call `kstep_sleep()`. This puts the driver thread to sleep via `usleep_range()`, allowing CPU 0 to enter the idle path. The kernel's `__update_idle_core()` finds all of core 0's SMT siblings (CPU 0 and CPU 1) idle, and sets `has_idle_cores = 1` on CPU 0's `sd_llc_shared`. When the sleep timer fires, the driver resumes on CPU 0.

6. **Widen task affinity**: Call `kstep_task_pin(task, 0, 3)` to set `p->cpus_ptr = {0, 1, 2, 3}`. This is the full set of online CPUs, including domain-isolated ones. The task is still blocked, so this only changes the affinity mask.

7. **Wake the task**: Call `kstep_task_wakeup(task)`. This executes `try_to_wake_up()` on CPU 0. The wakeup path is: `select_task_rq_fair()` → `select_idle_sibling(p, prev=0, target=0)` → early returns all fail → `select_idle_cpu(p, sd, has_idle_core=1, target=0)` → `select_idle_core(p, core=0, cpus={0}, &idle_cpu)`.

8. **Bug manifests inside `select_idle_core()`**: The function iterates `cpu_smt_mask(0) = {0, 1}`. CPU 0 is busy (running the wakeup code), so `idle = false` and the function looks for a fallback `*idle_cpu`. CPU 1 is fully idle (`available_idle_cpu(1)` returns true). The buggy check `cpumask_test_cpu(1, p->cpus_ptr)` passes because CPU 1 IS in the task's affinity. So `*idle_cpu = 1`. The function returns -1 (no fully idle core), but `idle_cpu = 1` is returned by `select_idle_cpu()` as the wakeup target.

9. **Check placement**: Advance one tick with `kstep_tick()`, then read `task_cpu(task)`.

### Detection Criteria

- **Buggy kernel**: `task_cpu(task) == 1`. The task is placed on CPU 1, a domain-isolated CPU. Inside `select_idle_core()`, the check `cpumask_test_cpu(1, p->cpus_ptr)` evaluated to true because CPU 1 is in the task's full affinity mask `{0, 1, 2, 3}`, even though CPU 1 is not in the domain-restricted `cpus = {0}`. Report: `kstep_fail("task placed on domain-isolated CPU %d", task_cpu(task))`.

- **Fixed kernel**: `task_cpu(task) == 0`. The fixed check `cpumask_test_cpu(1, cpus)` correctly fails because `cpus = sched_domain_span(sd) & p->cpus_ptr = {0}` does not include CPU 1. The `*idle_cpu` stays -1. `select_idle_cpu()` returns -1, `select_idle_sibling()` falls through and returns `target = 0`. The task is placed on CPU 0. Report: `kstep_pass("task correctly placed on CPU %d", task_cpu(task))`.

### Callbacks

Use `on_tick_begin` to log task placement after each tick for diagnostic tracing:
```c
static void on_tick_begin(void) {
    kstep_output_curr_task();
}
```

### Internal State Access (READ ONLY)

All internal state access is read-only, used purely for observation and verification:
- `task_cpu(p)` — read the CPU the task was placed on (primary detection signal)
- `cpu_rq(cpu)->curr` — optionally verify which task is running on each CPU after wakeup
- `cpu_rq(cpu)->nr_running` — optionally verify CPU 1 has a task enqueued (buggy) or not (fixed)
- `KSYM_IMPORT(sd_llc_shared)` + `per_cpu(KSYM_sd_llc_shared, 0)->has_idle_cores` — optionally read to confirm the flag was set naturally by `__update_idle_core()`, as a diagnostic assertion (not required for the test itself)

No writes to `has_idle_cores`, scheduling domain spans, or any other internal scheduler fields are performed. The flag is set entirely by the kernel's own idle entry path triggered via `kstep_sleep()`.

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
    // Step 1: Pin task to CPU 0 so prev_cpu = 0 after it runs
    kstep_task_pin(task, 0, 0);
    kstep_task_wakeup(task);
    kstep_tick_repeat(5);

    // Step 2: Block the task on CPU 0
    kstep_task_block(task);
    kstep_tick();

    // Step 3: Let CPU 0 go idle to naturally set has_idle_cores
    // CPU 1 (SMT sibling) is already idle, so __update_idle_core()
    // will set has_idle_cores = 1 when CPU 0 enters idle
    kstep_sleep();

    // Step 4: Widen affinity to all CPUs (including domain-isolated ones)
    kstep_task_pin(task, 0, 3);

    // Step 5: Wake the task — processed on CPU 0 with prev_cpu=0
    // Wakeup path: select_idle_sibling -> select_idle_cpu -> select_idle_core
    // Bug: select_idle_core checks p->cpus_ptr instead of domain-restricted cpus
    kstep_task_wakeup(task);
    kstep_tick();

    // Step 6: Check which CPU the task was placed on
    int cpu = task_cpu(task);
    if (cpu == 1 || cpu == 3) {
        kstep_fail("task placed on domain-isolated CPU %d (bug: "
                   "select_idle_core used p->cpus_ptr instead of cpus)", cpu);
    } else {
        kstep_pass("task correctly avoided domain-isolated CPUs, "
                   "placed on CPU %d", cpu);
    }
}

KSTEP_DRIVER_DEFINE {
    .name = "lb_idle_core_isolcpus",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_curr_task,
};
```

### Why No Writes to Internal State Are Needed

The previous strategy considered manually setting `sd_llc_shared->has_idle_cores` via `KSYM_IMPORT()`. This is unnecessary because the kernel sets the flag naturally through its own idle entry path. The sequence `kstep_task_block()` → `kstep_sleep()` reliably triggers `__update_idle_core()` on CPU 0 when no runnable tasks remain. Since CPU 1 (the domain-isolated SMT sibling) is always idle (no tasks are ever placed there), the `for_each_cpu(cpu, cpu_smt_mask(core))` loop in `__update_idle_core()` finds all siblings idle and calls `set_idle_cores(core, 1)`. The flag persists across the driver's own wakeup because `select_idle_sibling()` returns CPU 0 immediately (it is idle at that moment) without entering `select_idle_cpu()`, which is the only function that clears the flag. This natural approach is both more realistic and avoids any internal state manipulation.
