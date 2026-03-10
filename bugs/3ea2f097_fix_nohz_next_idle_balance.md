# Fix NOHZ next idle balance

- **Commit:** 3ea2f097b17e13a8280f1f9386c331b326a3dbef
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NOHZ load balancing (Fair scheduling)

## Bug Description

In systems with NOHZ (tickless idle) enabled, the rebalancing of the local CPU's scheduling domains occurs before other idle CPUs have had a chance to update `nohz.next_balance`. This causes the local CPU to overwrite the next idle balance time set by other idle CPUs with an incorrect value, leading to improper scheduling of future idle load balancing operations and potential scheduling imbalances across CPUs.

## Root Cause

The bug occurs because the order of operations in the NOHZ idle balance function was incorrect. After commit b7031a02ec75, the local CPU would rebalance its domains and update `nohz.next_balance` at the end of the function, but by that time it would overwrite the values that other idle CPUs were still in the process of setting. Additionally, `nohz.next_balance` was being incremented unconditionally even when only statistics updates were being requested, rather than full load balancing operations.

## Fix Summary

The fix reorders the operations so that `nohz.next_balance` is updated before the local CPU performs its rebalancing, ensuring that the value reflects the next time any idle CPU needs to run load balancing. Additionally, `nohz.next_balance` is now updated conditionally—only when a full idle load balance is being kicked, not when merely updating statistics.

## Triggering Conditions

The bug is triggered in NOHZ (CONFIG_NO_HZ_COMMON) systems when multiple idle CPUs participate in load balancing:

- Multiple CPUs must be idle and part of the NOHZ idle mask 
- NOHZ_STATS_KICK flag is used for stats-only updates vs NOHZ_BALANCE_KICK for full balancing
- Race condition occurs when the chosen idle load balancer (ILB) CPU calls `_nohz_idle_balance()`
- The local CPU updates `nohz.next_balance` at the end of `_nohz_idle_balance()` after other idle CPUs have already set their next balance times
- Timing-sensitive: requires concurrent idle balance operations across multiple CPUs
- The bug manifests as incorrect `nohz.next_balance` values, causing premature or delayed future idle load balancing

## Reproduce Strategy (kSTEP)

Use at least 3 CPUs (CPU 0 reserved for driver, need 2+ worker CPUs to create idle imbalance):

1. **Setup:** Create tasks on some CPUs but leave others idle using `kstep_task_create()` and `kstep_task_pin()`
2. **Topology:** Configure multi-CPU topology with `kstep_topo_init()`, `kstep_topo_set_mc()` to enable NOHZ balancing
3. **Trigger conditions:** Use `kstep_tick_repeat()` to advance time and create load imbalance, then make most CPUs idle
4. **Monitor:** Use `on_sched_softirq_begin()` callback to trace NOHZ idle balance kicks and `nohz.next_balance` updates
5. **Detection:** Log `nohz.next_balance` values before/after `_nohz_idle_balance()` calls. In buggy kernel, observe incorrect overwrites of next_balance times when stats-only kicks occur or when local rebalancing happens after other CPUs update the value
6. **Verification:** Compare with fixed kernel to ensure `nohz.next_balance` is set correctly and conditionally
