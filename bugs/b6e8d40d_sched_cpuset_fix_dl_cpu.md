# sched, cpuset: Fix dl_cpu_busy() panic due to empty cs->cpus_allowed

- **Commit:** b6e8d40d43ae4dec00c8fea2593eeea3114b8f44
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** Deadline scheduling, cpuset integration

## Bug Description

A kernel panic occurs when cgroup v2 has an empty cpus_allowed mask in a cpuset, causing task_can_attach() to pass an empty mask to cpumask_any_and(). This function returns nr_cpu_ids (out-of-bounds value) when no intersection exists, which is then used as a CPU index in dl_bw_of() to access percpu data, resulting in a page fault and system crash.

## Root Cause

The bug occurs because cpumask_any_and(cpu_active_mask, empty_mask) returns nr_cpu_ids instead of a valid CPU index. In cgroup v2, cpus_allowed can be legitimately empty (indicating inheritance from parent cpuset), but the code was not prepared to handle this case. The returned out-of-bounds value is directly used as an array index to access percpu bandwidth data, causing an invalid memory access.

## Fix Summary

The fix uses effective_cpus instead of cpus_allowed (which represents the actual usable CPUs for tasks in the cpuset), and adds a bounds check to guard against cpumask_any_and() returning nr_cpu_ids. This prevents the out-of-bounds percpu access that caused the panic.

## Triggering Conditions

The bug occurs in the deadline scheduler during cpuset task migration when:
- A cgroup v2 cpuset has an empty cpus_allowed mask (inheriting from parent)
- A deadline task is being migrated to this cpuset via cpuset_can_attach()
- The task's current runqueue domain doesn't intersect with the target cpuset
- cpumask_any_and(cpu_active_mask, empty_mask) returns nr_cpu_ids (invalid CPU)
- dl_cpu_busy() uses this invalid CPU index to access percpu dl_bw data
- This triggers a page fault when accessing out-of-bounds percpu memory

## Reproduce Strategy (kSTEP)

Need 2+ CPUs. Create a deadline task and trigger cpuset migration with empty mask:
- Use kstep_cgroup_create() to create nested cgroups with cpuset controller
- Use kstep_cgroup_set_cpuset() to create parent with non-empty, child with empty mask  
- Create deadline task with kstep_task_create() + set RT priority
- Pin task to CPU 1 initially using kstep_task_pin()
- Use kstep_cgroup_add_task() to migrate task to the empty-mask cpuset
- Monitor for kernel panic in dl_cpu_busy() via crash detection
- Use on_tick_begin() callback to log cpuset state and task migrations
- Verify bug by observing page fault at dl_bw_of() percpu access
- On fixed kernel, migration should succeed without panic
