# Fix clearing of has_idle_cores flag in select_idle_cpu()

- **Commit:** 02dbb7246c5bbbbe1607ebdc546ba5c454a664b1
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

In the `select_idle_cpu()` function, when searching for idle cores in the target CPU's Last Level Cache (LLC), if no idle core is found, the code clears the `has_idle_cores` flag to prevent other CPUs from wasting cycles searching the same LLC. However, the flag was being cleared for the *current* CPU's LLC instead of the *target* CPU's LLC, causing the optimization to fail and other CPUs to unnecessarily search LLCs that have already been determined to have no idle cores.

## Root Cause

The `set_idle_cores()` function call uses the wrong CPU variable. The code was passing `this` (the current CPU performing the search) instead of `target` (the CPU whose LLC is actually being searched). Since `has_idle_cores` is a per-LLC property indexed by CPU, clearing the flag for the wrong CPU means the flag for the target CPU's LLC remains set despite the failed search.

## Fix Summary

Change the variable passed to `set_idle_cores()` from `this` to `target` on line 6220 of kernel/sched/fair.c. This ensures that when no idle core is found in the target CPU's LLC, the `has_idle_cores` flag is cleared for the correct LLC, preventing unnecessary future searches of the same cache domain.

## Triggering Conditions

This bug is triggered during CPU selection in the CFS scheduler when:
- A task wakeup initiates `select_idle_cpu()` path for idle CPU selection
- The search targets an LLC domain different from the current CPU's LLC  
- `has_idle_cores` flag is set for the target LLC, enabling idle core search
- `select_idle_core()` fails to find an idle core in the target LLC
- Multiple CPUs repeatedly search the same target LLC due to incorrectly preserved `has_idle_cores` flag
- The overhead becomes measurable under high-frequency task wakeup workloads across different LLC domains

## Reproduce Strategy (kSTEP)

Setup CPU topology with 2+ LLC domains (e.g., 4 CPUs with LLC domains [1-2] and [3-4]):
- Use `kstep_topo_set_cls()` to define separate LLC domains for CPU pairs
- Create multiple tasks that will cause cross-LLC wakeups  
- Pin initial tasks to create imbalance: heavy load on CPUs 1-2, lighter on 3-4
- In `run()`: repeatedly wakeup tasks targeting the lighter LLC while current CPU is in heavy LLC
- Use `on_sched_balance_begin()` callback to log when `select_idle_cpu()` is called across LLCs
- Monitor repeated searches of same target LLC with failed `select_idle_core()` calls
- Detect bug by counting excessive LLC searches that should have been prevented by cleared `has_idle_cores` flag
