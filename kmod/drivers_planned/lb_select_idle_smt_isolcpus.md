# LB: select_idle_smt() Ignores Scheduling Domain with isolcpus

**Commit:** `8aeaffef8c6eceab0e1498486fdd4f3dc3b7066c`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.9-rc1 (merged via v6.8-rc4)
**Buggy since:** v6.1-rc1 (introduced by commit `3e6efe87cd5c` "sched/fair: Remove redundant check in select_idle_smt()")

## Bug Description

When a task wakes up, the CFS scheduler calls `select_idle_sibling()` to find a
suitable idle CPU for the task. One of the helpers in this path is
`select_idle_smt()`, which scans the SMT siblings of a given CPU for an idle
sibling to place the waking task on. The bug is that `select_idle_smt()` does
not check whether a candidate SMT sibling CPU actually belongs to the target
CPU's scheduling domain. This is a problem when the `isolcpus=domain` kernel
boot parameter has been used to isolate certain CPUs from the scheduling domain,
because an isolated CPU's SMT sibling may still be in the domain. In that case,
`select_idle_smt()` can select the isolated CPU, violating the CPU isolation
guarantee.

The root cause is a regression introduced by commit `3e6efe87cd5c` ("sched/fair:
Remove redundant check in select_idle_smt()"). That commit assumed that if two
CPUs share an LLC cache (a prerequisite for `select_idle_smt()` being called),
their SMT siblings must also be in the same LLC scheduling domain. This
assumption is incorrect when `isolcpus=domain` removes specific CPUs from the
domain: the hardware topology still shows them as SMT siblings, but the
scheduling domain no longer includes them.

An earlier commit `df3cb4ea1fb6` ("sched/fair: Fix wrong cpu selecting from
isolated domain") had correctly added a check against `sched_domain_span(sd)` in
the `select_idle_smt()` loop. Commit `3e6efe87cd5c` removed this check as part
of a cleanup, believing it was redundant. The fix commit `8aeaffef8c6e` restores
the scheduling domain check.

## Root Cause

The function `select_idle_smt()` in `kernel/sched/fair.c` is called from
`select_idle_sibling()` during the CFS task wakeup path. Its purpose is to scan
the SMT siblings of the `prev` CPU (the CPU the task last ran on) for an idle
CPU. The buggy version of the function looks like this:

```c
static int select_idle_smt(struct task_struct *p, int target)
{
    int cpu;
    for_each_cpu_and(cpu, cpu_smt_mask(target), p->cpus_ptr) {
        if (cpu == target)
            continue;
        if (available_idle_cpu(cpu) || sched_idle_cpu(cpu))
            return cpu;
    }
    return -1;
}
```

The iteration uses `for_each_cpu_and(cpu, cpu_smt_mask(target), p->cpus_ptr)`,
which iterates over CPUs that are both SMT siblings of `target` AND in the
task's CPU affinity mask (`p->cpus_ptr`). It does NOT check whether the
candidate CPU is in the scheduling domain of `target`.

The scheduling domain (`sd`) for a CPU defines the set of CPUs that the
scheduler considers for load balancing and task placement. When
`isolcpus=domain,X` is specified on the kernel command line, CPU X is removed
from all scheduling domains. This means `sched_domain_span(sd)` for any
non-isolated CPU will NOT include CPU X.

However, `cpu_smt_mask(target)` is based on the hardware topology and always
includes all SMT siblings regardless of domain configuration. Similarly,
`p->cpus_ptr` reflects the task's affinity, which may include the full CPU set
if the task was placed in a permissive cpuset or given full affinity.

The specific call site in `select_idle_sibling()` is:

```c
if (sched_smt_active()) {
    has_idle_core = test_idle_cores(target);
    if (!has_idle_core && cpus_share_cache(prev, target)) {
        i = select_idle_smt(p, prev);  /* buggy: no sd parameter */
        if ((unsigned int)i < nr_cpumask_bits)
            return i;
    }
}
```

The `sd` (LLC scheduling domain) has already been looked up earlier in
`select_idle_sibling()` via `rcu_dereference(per_cpu(sd_llc, target))`, but the
buggy `select_idle_smt()` does not receive or use it. This means the function
has no way to filter out CPUs that are not in the domain.

The original fix in commit `df3cb4ea1fb6` correctly passed `sd` to
`select_idle_smt()` and checked `cpumask_test_cpu(cpu, sched_domain_span(sd))`
in the loop. Commit `3e6efe87cd5c` removed this check, reasoning that if
`cpus_share_cache(prev, target)` is true, then all siblings must also be in the
same LLC domain — which is wrong when `isolcpus=domain` is active.

## Consequence

The observable impact is that tasks are incorrectly placed on CPUs that were
explicitly isolated from the scheduling domain via `isolcpus=domain`. This
violates the administrator's intent to keep those CPUs free from general task
scheduling. In production environments, `isolcpus=domain` is commonly used to
reserve CPUs for latency-sensitive real-time workloads, dedicated interrupt
handling, or DPDK-style polling. Having arbitrary tasks scheduled on these
isolated CPUs causes:

1. **Isolation violation**: Tasks that should never run on isolated CPUs end up
   there, defeating the purpose of CPU isolation.
2. **Performance degradation**: The isolated CPUs may be dedicated to specific
   workloads (e.g., real-time threads, network packet processing). Interference
   from unrelated tasks causes unpredictable latency spikes.
3. **Workload interference**: On SMT systems, sharing a physical core between an
   isolated workload and an unexpected task causes cache and pipeline contention.

The original reporter at Alibaba observed that on a 31-CPU hyperthreads machine
with `isolcpus=domain,2-31` (isolating all but CPUs 0 and 1, where threads
0/16 and 1/17 form SMT pairs), tasks placed in a cpuset (e.g., via
`cgcreate -g cpu:test; cgexec -g cpu:test "test_threads"`) would occasionally
be migrated to isolated CPUs 16-17 (the SMT siblings of non-isolated CPUs 0 and
1). There is no crash or kernel panic — the bug silently places tasks on the
wrong CPUs.

## Fix Summary

The fix in commit `8aeaffef8c6eceab0e1498486fdd4f3dc3b7066c` restores the
scheduling domain check that was accidentally removed. The changes are:

1. **Function signature change**: `select_idle_smt()` gains a `struct
   sched_domain *sd` parameter (in both the SMT and non-SMT stubs):
   ```c
   -static int select_idle_smt(struct task_struct *p, int target)
   +static int select_idle_smt(struct task_struct *p, struct sched_domain *sd, int target)
   ```

2. **Domain check added to the loop**: Inside the SMT sibling iteration, a check
   is added to skip CPUs not in the scheduling domain:
   ```c
   for_each_cpu_and(cpu, cpu_smt_mask(target), p->cpus_ptr) {
       if (cpu == target)
           continue;
   +   if (!cpumask_test_cpu(cpu, sched_domain_span(sd)))
   +       continue;
       if (available_idle_cpu(cpu) || sched_idle_cpu(cpu))
           return cpu;
   }
   ```

3. **Call site updated**: The call in `select_idle_sibling()` passes the `sd`
   that was already looked up:
   ```c
   -i = select_idle_smt(p, prev);
   +i = select_idle_smt(p, sd, prev);
   ```

The fix is correct because `sd` is the LLC scheduling domain for `target`,
which is guaranteed to be valid at this point (there is an early return if `sd`
is NULL). By checking `sched_domain_span(sd)`, the function ensures that only
CPUs actually participating in the scheduling domain are considered. This
correctly filters out CPUs isolated via `isolcpus=domain`.

The non-SMT stub (`#else` branch) also receives the `sd` parameter for API
consistency, but always returns -1 since there are no SMT siblings to scan.

## Triggering Conditions

The following conditions must all be met simultaneously to trigger this bug:

1. **SMT (Hyperthreading) must be active**: The system must have SMT-capable
   CPUs with at least 2 threads per core. `sched_smt_active()` must return true.
   On x86, this means Intel Hyperthreading or AMD SMT must be enabled.

2. **`isolcpus=domain` must isolate specific CPUs**: The kernel must be booted
   with `isolcpus=domain,...` where the isolated CPU set includes at least one
   SMT sibling of a non-isolated CPU. For example, on a system where CPUs 0 and
   1 are SMT siblings, `isolcpus=domain,1` creates the vulnerable configuration:
   CPU 0 is in the domain, CPU 1 is not, but both are hardware SMT siblings.

3. **Task must have the isolated CPU in its affinity mask**: The waking task's
   `cpus_ptr` must include the isolated CPU. This happens when:
   - The task has full/default affinity (all CPUs allowed)
   - The task is in a cpuset that includes the isolated CPU
   - The task's affinity was explicitly set to include isolated CPUs

4. **No idle cores available**: `test_idle_cores(target)` must return false
   (i.e., `has_idle_core == false`). If there are idle cores, `select_idle_cpu()`
   is called instead, and `select_idle_smt()` is skipped.

5. **prev and target must share LLC cache**: `cpus_share_cache(prev, target)`
   must be true. This is typically the case when prev and target are in the same
   LLC domain.

6. **The isolated SMT sibling must be idle**: The isolated CPU must be idle
   (`available_idle_cpu()` returns true) or running only SCHED_IDLE tasks
   (`sched_idle_cpu()` returns true). Since isolated CPUs typically have no tasks
   scheduled on them, they are almost always idle, making this condition trivially
   satisfied.

The bug is highly reproducible once the topology and isolation conditions are
met, because isolated CPUs are inherently idle and will always be selected by
the buggy `select_idle_smt()`. There is no race condition or timing sensitivity
— the bug triggers deterministically on every task wakeup that enters the
`select_idle_smt()` path with a prev CPU whose sibling is isolated.

## Reproduce Strategy (kSTEP)

### Overview

This bug can be reproduced in kSTEP with **minor extensions** to the framework.
The core idea is to boot QEMU with an SMT topology and `isolcpus=domain` to
create a configuration where an SMT sibling is excluded from the scheduling
domain. Then, trigger a task wakeup and observe whether the task gets placed on
the isolated CPU (buggy) or not (fixed).

### Required kSTEP Extensions

Two minor extensions to kSTEP are needed:

1. **SMT topology in QEMU**: The current `run_qemu()` in `run.py` uses
   `-smp {num_cpus}` which creates CPUs without explicit SMT topology (1 thread
   per core). Add support for specifying SMT topology, e.g.:
   ```python
   # In Driver class:
   smp_topology: str = ""  # e.g., "cores=2,threads=2,sockets=1"
   ```
   Then in `run_qemu()`, use `-smp {topology}` instead of `-smp {num_cpus}`
   when the field is set.

2. **Custom isolcpus configuration**: The current `run_qemu()` hardcodes
   `isolcpus=nohz,managed_irq,{isol_cpus}`. Add support for overriding the
   isolcpus parameter:
   ```python
   # In Driver class:
   isolcpus_override: str = ""  # e.g., "domain,nohz,managed_irq,2-3"
   ```
   When set, use this instead of the default isolcpus configuration.

### QEMU and Boot Configuration

- **CPUs**: 4 CPUs with SMT: 2 cores × 2 threads per core × 1 socket.
  QEMU argument: `-smp 4,cores=2,threads=2,sockets=1`.
  This creates the topology:
  - Core 0: CPU 0, CPU 2 (SMT siblings)
  - Core 1: CPU 1, CPU 3 (SMT siblings)

- **isolcpus**: `isolcpus=domain,nohz,managed_irq,2-3`.
  This isolates CPUs 2 and 3 from the scheduling domain AND from nohz/managed_irq.
  The scheduling domain for CPUs 0-1 will contain only {0, 1}.
  CPUs 2 and 3 are still present in hardware topology (SMT siblings of 0 and 1)
  but excluded from `sched_domain_span(sd)`.

- **CPU 0**: Reserved for the kSTEP driver (as usual).
- **CPU 1**: Available for scheduled tasks (in the domain).
- **CPUs 2-3**: Isolated from domain (SMT siblings of CPUs 0 and 1 respectively).

### Driver Implementation

```
setup():
  // Topology is already configured via QEMU boot params and isolcpus
  // No kstep_topo calls needed since QEMU handles SMT topology natively

  // Create a CFS task
  task = kstep_task_create()

  // Pin the task to CPUs 1-3 (includes both domain and isolated CPUs)
  // This ensures p->cpus_ptr includes the isolated CPU 3
  kstep_task_pin(task, 1, 3)

run():
  // Wake the task, let it run on CPU 1
  kstep_task_wakeup(task)
  kstep_tick_repeat(5)  // Let scheduler settle, task runs on CPU 1

  // Record which CPU the task is on
  int cpu_before = task_cpu(task)

  // Block the task
  kstep_task_block(task)
  kstep_tick_repeat(2)  // Let the block take effect

  // Ensure CPU 1 is busy so there's no idle core
  // (We need !has_idle_core for select_idle_smt to be called)
  // Create a second task pinned to CPU 1 to keep it busy
  task2 = kstep_task_create()
  kstep_task_pin(task2, 1, 1)
  kstep_task_wakeup(task2)
  kstep_tick_repeat(2)

  // Now wake the original task
  // select_idle_sibling() will be called:
  //   prev = CPU 1 (last ran there)
  //   target = some CPU (likely CPU 1 or the waker's CPU)
  //   sd = LLC domain containing {0, 1}
  //   sched_smt_active() = true (SMT topology)
  //   has_idle_core = false (CPU 1 is busy with task2, CPU 0 is driver)
  //   cpus_share_cache(prev, target) = true
  //   => select_idle_smt(p, sd, prev=1) is called
  //   => Iterates CPU 1's sibling: CPU 3
  //   => Buggy: CPU 3 is idle, returns CPU 3
  //   => Fixed: CPU 3 not in domain, skips, returns -1
  kstep_task_wakeup(task)
  kstep_tick_repeat(3)

  // Check which CPU the task ended up on
  int cpu_after = task_cpu(task)

  // On buggy kernel: cpu_after == 3 (isolated CPU selected)
  // On fixed kernel: cpu_after should be 0 or 1 (within domain)
  if (cpu_after == 2 || cpu_after == 3) {
      kstep_fail("Task placed on isolated CPU %d (expected CPU in domain {0,1})", cpu_after)
  } else {
      kstep_pass("Task correctly placed on CPU %d (within scheduling domain)", cpu_after)
  }
```

### Callbacks

No special callbacks (`on_tick_begin`, `on_sched_softirq_end`, etc.) are needed
for the basic reproduction. However, for enhanced debugging:

- **`on_tick_begin`**: Log `task_cpu(task)` on each tick to track task migration.
- **Logging**: Use `KSYM_IMPORT` to import `sched_domain_span` if needed for
  introspection, or read `per_cpu(sd_llc, cpu)` to verify domain configuration.

### Detection Criteria

- **Bug present (buggy kernel)**: After wakeup, `task_cpu(task)` returns 2 or 3
  (an isolated CPU). Specifically, CPU 3 (the SMT sibling of CPU 1, which is
  isolated from the domain) is the most likely target.
- **Bug absent (fixed kernel)**: After wakeup, `task_cpu(task)` returns 0 or 1
  (a CPU within the scheduling domain). The task may land on CPU 0 if it's idle,
  or remain on CPU 1's run queue if no other idle CPU is available.

### Determinism

This reproduction is highly deterministic because:
1. Isolated CPUs are always idle (no tasks are scheduled there by the domain).
2. `select_idle_smt()` always iterates SMT siblings in a fixed order.
3. The conditions for entering `select_idle_smt()` are controlled by the setup
   (keeping CPU 1 busy ensures `!has_idle_core`).
4. The task's affinity explicitly includes the isolated CPU.

### Alternative Approach (No isolcpus Extension)

If modifying kSTEP's boot parameters is undesirable, an alternative approach
could use `KSYM_IMPORT` to directly inspect the `sched_domain` structures and
verify the presence or absence of the domain check. However, this would only
verify the code path exists, not that the bug triggers observable misbehavior.
The `isolcpus=domain` approach is strongly preferred as it creates real, observable
misplacement of tasks.
