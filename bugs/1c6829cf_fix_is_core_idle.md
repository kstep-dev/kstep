# sched/numa: Fix is_core_idle()

- **Commit:** 1c6829cfd3d5124b125e6df41158665aea413b35
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA, SMT (Simultaneous Multi-Threading)

## Bug Description

The `is_core_idle()` function incorrectly checks whether a CPU core is idle by testing only the input CPU parameter repeatedly in a loop, rather than testing each SMT sibling. This causes the function to fail to detect when sibling CPUs are busy, leading to incorrect core idle detection and potentially causing the scheduler to treat a busy core as idle when making NUMA migration decisions.

## Root Cause

The function iterates through SMT siblings using `for_each_cpu(sibling, cpu_smt_mask(cpu))` but calls `idle_cpu(cpu)` inside the loop instead of `idle_cpu(sibling)`. This means the check uses the function's parameter (the input CPU) rather than the loop variable (each sibling), causing the same CPU to be checked repeatedly and all other siblings to be ignored.

## Fix Summary

Change `idle_cpu(cpu)` to `idle_cpu(sibling)` in the loop body so that the function correctly checks if each SMT sibling is idle rather than repeatedly checking only the input CPU.

## Triggering Conditions

The bug requires an SMT-enabled system where `is_core_idle()` is called during NUMA scheduling decisions. Specific conditions:
- SMT topology configured (hyperthreading enabled with multiple threads per core)
- Uneven task distribution where some SMT siblings are busy while others are idle
- NUMA migration scenarios that invoke `is_core_idle()` to check target core availability
- The function will incorrectly return `true` (core idle) even when non-queried siblings are busy
- Race condition: the queried CPU must be idle while its SMT siblings have running tasks
- Affects NUMA load balancing and task placement decisions in multi-socket systems

## Reproduce Strategy (kSTEP)

Use 4+ CPUs with SMT topology to create scenarios where `is_core_idle()` gives wrong results:
- **Setup**: Configure SMT pairs via `kstep_topo_set_smt()` (e.g., CPUs 1-2, 3-4 as SMT pairs)
- **Tasks**: Create 3+ tasks using `kstep_task_create()` and pin them strategically
- **Scenario**: Pin one task to CPU 1, another to CPU 3, leave CPU 2 idle initially
- **Trigger**: Use `kstep_tick_repeat()` to advance scheduler state, then migrate a task
- **Observation**: Hook `on_sched_balance_begin()` to catch NUMA balance operations
- **Detection**: Add custom logging in the driver to call `is_core_idle()` directly and compare results before/after fix
- **Validation**: Verify that busy siblings (CPU 2's sibling CPU 1) cause incorrect idle detection
- **Expected**: Pre-fix version reports core as idle even when siblings are busy
