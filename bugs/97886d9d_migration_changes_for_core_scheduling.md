# sched: Migration changes for core scheduling

- **Commit:** 97886d9dcd86820bdbc1fa73b455982809cbc8c2
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** core scheduling

## Bug Description

The scheduler was not considering core scheduling cookies when making task migration and CPU placement decisions. Without cookie matching checks, the scheduler would migrate tasks to CPUs with mismatched cookies or select idle CPUs that don't match the task's cookie. This resulted in forced idle time on destination cores, as the cores would have to remain idle when scheduled tasks' cookies didn't match the core's cookie, causing unnecessary performance degradation.

## Root Cause

The task placement logic in load balancing, idle CPU selection, and group selection routines (`find_idlest_group_cpu()`, `__select_idle_cpu()`, `task_hot()`, `find_idlest_group()`) was not checking whether a task's cookie matched the destination CPU's core cookie before making placement decisions. This oversight meant core scheduling constraints were being ignored during scheduling operations.

## Fix Summary

The fix adds three helper functions (`sched_cpu_cookie_match()`, `sched_core_cookie_match()`, `sched_group_cookie_match()`) and integrates cookie matching checks throughout the task placement logic. These checks ensure tasks are only migrated to or placed on CPUs with matching core cookies, and idle CPU selection prioritizes cookie-matched idle CPUs, thereby reducing forced idle time.

## Triggering Conditions

The bug occurs when core scheduling is enabled and tasks with different security cookies are present in the system. Specifically:
- Core scheduling must be enabled with tasks having different cookies assigned
- Task wakeup or migration triggers CPU selection via `find_idlest_group_cpu()` or `__select_idle_cpu()`
- Available idle or lightly-loaded CPUs exist with cookies that don't match the task's cookie
- The scheduler selects these mismatched CPUs based solely on load/idle state, ignoring cookie compatibility
- This results in task placement on cores where the task cannot run alongside sibling core tasks due to cookie mismatch
- The destination core is forced idle when core scheduling constraints prevent execution
- Performance degradation occurs due to unnecessary forced idle time and suboptimal task placement

## Reproduce Strategy (kSTEP)

Configure a 4-CPU system with SMT pairs (CPUs 1-2 and 3-4 sharing cores). In `setup()`:
- Use `kstep_topo_init()` and `kstep_topo_set_smt()` to establish SMT topology
- Create tasks with different security cookies (requires manual cookie assignment in driver)
- Create tasks: one high-priority task for CPU 1, multiple tasks for CPU 3

In `run()`:
- Pin high-priority task to CPU 1, ensuring CPU 2 (sibling) has different cookie
- Pin multiple tasks to CPU 3, making it heavily loaded
- Use `kstep_task_wakeup()` to wake a task with CPU 1's cookie while CPU 2 is idle
- Without the fix, scheduler selects idle CPU 2 despite cookie mismatch
- Use `on_tick_begin()` callback with `kstep_output_nr_running()` to track CPU utilization
- Detect bug by observing task placement on mismatched CPUs and resulting forced idle cycles
- Log cookie mismatches and forced idle events to confirm bug manifestation
