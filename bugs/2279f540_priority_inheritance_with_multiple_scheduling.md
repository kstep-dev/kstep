# sched/deadline: Fix priority inheritance with multiple scheduling classes

- **Commit:** 2279f540ea7d05f22d2f0c4224319330228586bc
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c
- **Subsystem:** Deadline

## Bug Description

A kernel BUG (crash) is triggered in `enqueue_task_dl()` when a SCHED_DEADLINE task contends with CFS tasks through nested PTHREAD_PRIO_INHERIT mutexes. The issue occurs when a CFS task boosted by a SCHED_DEADLINE task boosts another CFS task (nested priority inheritance), causing `replenish_dl_entity()` to access null/uninitialized deadline parameters and trigger a BUG at `kernel/sched/deadline.c:1462`.

## Root Cause

When a non-DEADLINE task is boosted through nested priority inheritance chains, the boosted task's deadline parameters remain uninitialized (set to 0 at fork time). The original code used a simple boolean flag (`dl_boosted`) to indicate boosting but did not track which scheduling entity's parameters should be used for replenishment. When a boosted non-DEADLINE task's deadline parameters were checked, the code would hit a BUG_ON condition because `pi_se->dl_runtime` was 0.

## Fix Summary

The fix replaces the boolean `dl_boosted` flag with a pointer `pi_se` that tracks the original priority inheritance donor's scheduling entity. New helper functions `pi_of()` and `is_dl_boosted()` encapsulate access to this information. All deadline replenishment and overflow checks now use `pi_of(dl_se)` to access the actual donor's scheduling parameters, ensuring correct behavior in nested priority inheritance scenarios across multiple scheduling classes.

## Triggering Conditions

The bug requires nested priority inheritance chains involving both DEADLINE and CFS tasks. Specifically: (1) A CFS task N1 holds mutex M1, (2) Another CFS task N2 holds mutex M2 and blocks on M1, (3) A SCHED_DEADLINE task D1 blocks on M2, boosting N2 to DEADLINE priority, (4) N2's blocking on M1 transitively boosts N1 to DEADLINE priority. The critical condition is that N1 (originally CFS) has uninitialized deadline parameters (dl_runtime = 0) but gets enqueued as a DEADLINE task. When `replenish_dl_entity()` is called during N1's enqueue, it accesses N1's null deadline parameters and hits the BUG_ON condition at line 1462 in deadline.c. The race requires precise timing where the nested boosting occurs before any of the tasks release their mutexes.

## Reproduce Strategy (kSTEP)

Use 3 CPUs minimum (CPU 0 reserved for driver). Create three tasks: `task_n1` and `task_n2` as CFS tasks, `task_d1` as SCHED_DEADLINE with proper dl_runtime/dl_period parameters. In `setup()`, configure task_d1 with `kstep_task_deadline()` (hypothetical API extension needed). In `run()`, implement the nested locking scenario: (1) Have task_n1 acquire mutex M1 via kernel synchronization primitives, (2) Have task_n2 acquire mutex M2 then block on M1, (3) Have task_d1 block on M2 to trigger the priority inheritance chain. Use `on_tick_begin()` callback to monitor task priority changes and detect when N1 gets boosted to DEADLINE class. Check for kernel BUG by monitoring task states and deadline parameters. The bug manifests as a kernel crash in `enqueue_task_dl()` when N1 with uninitialized deadline parameters gets enqueued as DEADLINE.
