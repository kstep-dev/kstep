# sched/debug: Make sd->flags sysctl read-only

- **Commit:** 9818427c6270a9ce8c52c8621026fe9cebae0f92
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** core

## Bug Description

Writing to the sysctl interface for a sched_domain's flags directly updates the field but bypasses the `update_top_cache_domain()` function. This causes cached domain pointers to become stale, as they were computed at the last sched_domain rebuild but no longer reflect the current state of the flags. When code performs explicit domain walks checking for flags, it sees the new values, but cached pointers still reference domains with outdated flag states, leading to synchronization inconsistencies.

## Root Cause

The sysctl write handler modifies `sd->flags` without triggering an update of the cached sched_domain pointers. These pointers are only refreshed during sched_domain rebuilds, so a direct sysctl write creates a mismatch where the in-memory flag value diverges from what the cached pointers expect, violating the invariant that cached pointers always refer to domains with consistent state.

## Fix Summary

The fix makes the sysctl interface read-only by changing the file permissions from 0644 (read-write) to 0444 (read-only). This eliminates the dangerous write path entirely rather than attempting to synchronize cached pointers on every write, which would be complex and error-prone.

## Triggering Conditions

This bug is triggered when a sched_domain's flags are modified via sysctl write after scheduler domain topology has been built. The specific conditions are:
- A multi-level scheduler domain topology must exist (e.g., SMT/MC/NUMA hierarchies)
- Code that relies on cached domain pointers (e.g., `sd_llc`, `sd_numa`, `sd_asym_packing`) must be active
- Direct write to `/proc/sys/kernel/sched_domain/cpuX/domainY/flags` via sysctl interface
- Subsequent scheduler operations that check flags via explicit domain walks vs. cached pointers
- The inconsistency manifests when flag-dependent logic uses stale cached pointers that no longer match the updated flag values, potentially causing load balancing decisions based on outdated domain characteristics

## Reproduce Strategy (kSTEP)

To reproduce this inconsistency, create a multi-level topology and trigger sched_domain operations:
- Use at least 4 CPUs (1 reserved for driver, 3+ for multi-level domains)
- In setup(): Use `kstep_topo_init()`, `kstep_topo_set_smt()`, `kstep_topo_set_cls()` to create SMT/MC hierarchy
- Create tasks with `kstep_task_create()` and pin them across domains with `kstep_task_pin()`
- In run(): Write to sysctl via `kstep_write()` to modify domain flags (e.g., disable SD_BALANCE_NEWIDLE)
- Trigger load balancing with `kstep_tick_repeat()` and task wake-ups via `kstep_task_wakeup()`
- Use `on_sched_balance_begin()` callback to log domain flags during explicit walks
- Compare logged flags with cached domain behavior to detect inconsistency
- Success: Load balancing decisions should match updated flags; failure indicates stale cached pointers
