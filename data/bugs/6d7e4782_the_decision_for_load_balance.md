# sched/fair: Fix the decision for load balance

- **Commit:** 6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

The `should_we_balance()` function determines which CPU should initiate load balancing during scheduler ticks. However, the buggy code allowed multiple CPUs to return true simultaneously, causing duplicate load-balancing operations. For example, in a scenario with SMT cores where configuration is `[0,1] [2,3]` with pattern `busy busy idle busy`, both CPU 0 (from the first core) and CPU 2 (idle with busy sibling) would incorrectly return true, violating the invariant that only one CPU should make the load-balancing decision.

## Root Cause

The bug occurs because the code checks `if (idle_smt == env->dst_cpu) return true;` without first validating that `idle_smt` was actually initialized (i.e., that an idle CPU with busy siblings was found). The variable `idle_smt` is initialized to -1, and when a valid idle CPU with busy siblings is found, it is assigned. However, the original code unconditionally returns true if `idle_smt` matches the current CPU, even when `idle_smt` was never set during the loop, allowing unintended CPUs to return true.

## Fix Summary

The fix adds a check `if (idle_smt != -1)` before using `idle_smt` to make the decision, ensuring that only when an idle CPU with busy siblings has been explicitly found does the code return based on the `idle_smt` value. This guarantees that only the first idle CPU with busy siblings will return true, preventing multiple CPUs from initiating load balancing in the same tick.

## Triggering Conditions

The bug requires an SMT topology with specific CPU states during load balancing. Key conditions:
- SMT cores (hyperthreading) with busy and idle CPU siblings on same core
- No completely idle cores available (all cores have at least one busy CPU)
- At least one idle CPU with busy siblings (e.g., core [2,3] with pattern "idle busy")
- Load balancing invocation during scheduler tick when `should_we_balance()` is called
- The `idle_smt` variable remains uninitialized (-1) for some CPUs due to search order
- Multiple CPUs can incorrectly match `idle_smt == env->dst_cpu` condition simultaneously

The classic example: topology `[0,1] [2,3]` with pattern `busy busy idle busy` where both CPU 0 and CPU 2 return true.

## Reproduce Strategy (kSTEP)

Requires at least 5 CPUs (0 reserved for driver). Set up SMT topology and create the problematic load pattern:

**Setup:** Use `kstep_topo_init()`, `kstep_topo_set_smt()` to create SMT pairs like `[1,2] [3,4]`, then `kstep_topo_apply()`

**Tasks:** Create 4 tasks, pin them to achieve pattern `busy busy idle busy` on CPUs 1,2,3,4:
- `kstep_task_pin(task1, 1, 1)` and `kstep_task_pin(task2, 2, 2)` for busy cores
- `kstep_task_pin(task3, 4, 4)` leaving CPU 3 idle with busy sibling

**Detection:** Use `on_sched_balance_selected()` callback to monitor which CPUs return true from `should_we_balance()`. Log via `kstep_output_balance()`. In buggy kernel, multiple CPUs will be selected for balancing in same tick, violating the single-CPU invariant.

**Execution:** Run `kstep_tick_repeat()` to trigger periodic load balancing and observe duplicate balance selections.
