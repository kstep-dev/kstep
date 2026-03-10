# Fix comparison in sched_group_cookie_match()

- **Commit:** e705968dd687574b6ca3ebe772683d5642759132
- **Affected file(s):** kernel/sched/sched.h
- **Subsystem:** Core scheduling

## Bug Description

The `sched_group_cookie_match()` function iterates over CPUs in an SMT group to check if any CPU has a matching core cookie. However, it was using the same `rq` parameter for all iterations instead of obtaining the runqueue for each CPU being checked. This causes incorrect cookie matching, potentially allowing tasks to be scheduled on incompatible cores and breaking core scheduling isolation guarantees.

## Root Cause

In the loop `for_each_cpu_and(cpu, sched_group_span(group), p->cpus_ptr)`, the function was calling `sched_core_cookie_match(rq, p)` with the original `rq` parameter for every iteration. Since `cpu` is the loop variable representing different CPUs in the group, the function should check the core cookie of each CPU's runqueue using `cpu_rq(cpu)`, not reuse the same `rq` for every check.

## Fix Summary

The fix changes the function call from `sched_core_cookie_match(rq, p)` to `sched_core_cookie_match(cpu_rq(cpu), p)` so that the core cookie check is performed on the correct runqueue for each CPU being iterated. Additionally, the commit moves the declarations and macros for `cpu_rq()` and related functions earlier in the header file to make them available for use in the core scheduling code.

## Triggering Conditions

The bug occurs during core scheduling when tasks with different security cookies are being scheduled. Core scheduling must be enabled and the system must have SMT topology (hyperthreading). The bug triggers when `sched_group_cookie_match()` is called during scheduling decisions to check if a task's cookie is compatible with any CPU in an SMT group. The incorrect cookie matching would allow security isolation violations, where tasks with incompatible cookies could be placed on sibling SMT cores, potentially enabling speculative execution attacks.

## Reproduce Strategy (kSTEP)

Setup requires at least 3 CPUs (CPU 0 reserved + 2 SMT siblings). Configure SMT topology using `kstep_topo_set_smt()` to create sibling pairs. Enable core scheduling via sysctl. Create tasks with different core cookies using cgroups and `kstep_cgroup_create()`. Use `kstep_task_pin()` to constrain tasks to specific CPUs, then trigger scheduling decisions with `kstep_tick_repeat()`. Monitor cookie matching behavior using `on_tick_begin()` callbacks to log current tasks and their cookies on each CPU. The bug manifests as incorrect cookie matching allowing incompatible tasks on SMT siblings, which would be detected by checking cookie relationships across sibling CPUs.
