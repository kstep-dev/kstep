# sched/numa: fix task swap by skipping kernel threads

- **Commit:** 9709eb0f845b713ba163f2c461537d8add3e4e04
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** NUMA

## Bug Description

During NUMA load balancing, when the NUMA load balancer decides to swap a local task with a remote task to improve NUMA locality, it may incorrectly select a kernel thread as the swap candidate. This is wrong because NUMA balancing only considers user-space memory accesses via VMAs, and kernel threads do not participate in NUMA balancing. As a result, kernel threads get unnecessarily migrated between nodes, wasting system resources and disrupting their normal operation.

## Root Cause

The task_numa_compare() function only checked for PF_EXITING and idle tasks (via is_idle_task()) before selecting a task as a swap candidate, but did not filter out kernel threads. Kernel threads are marked with PF_KTHREAD flag and lack memory management (no mm), so they should never be candidates for NUMA-aware swapping. The original code missed this critical check, allowing kernel threads to be selected as swap targets.

## Fix Summary

The fix adds two conditions to skip unsuitable tasks: it now checks for PF_KTHREAD flag to exclude kernel threads and also verifies that the task has a valid mm (memory management context) to handle user-mode threads created without mm. This aligns the task selection logic with how task_tick_numa() handles NUMA eligibility.

## Triggering Conditions

The bug is triggered in the NUMA load balancer's task_numa_compare() function when:
- NUMA balancing is enabled and active (sysctl_numa_balancing is set)
- A user task (task A) has a preferred NUMA node different from its current node
- Task A's preferred node has no idle CPUs available, forcing a task swap decision
- There exists a kernel thread (PF_KTHREAD set) or thread without mm on the preferred node
- The NUMA load balancer reaches task_numa_compare() and evaluates the kernel thread/no-mm thread as a potential swap candidate
- The original code lacks PF_KTHREAD and mm checks, allowing kernel threads to be selected for NUMA swapping

## Reproduce Strategy (kSTEP)

Use at least 2 CPUs with NUMA topology. Create NUMA nodes with kstep_topo_set_node().
In setup(), enable NUMA balancing via sysctl_numa_balancing=1. Create kernel threads using kstep_kthread_create() and bind them to specific CPUs on node 1 with kstep_kthread_bind().
Fill node 1 with both user tasks and kernel threads to eliminate idle CPUs.
In run(), create user tasks with kstep_task_create(), set their NUMA preferred node to node 1, but place them initially on node 0. Use kstep_tick_repeat() to trigger NUMA load balancing cycles.
Use on_tick_begin() or on_sched_softirq_end() callbacks to monitor task migration events and detect when kernel threads are being considered or migrated during NUMA task swapping.
Log task flags (PF_KTHREAD) and mm status of swap candidates to confirm the bug triggers when kernel threads are selected for NUMA balancing.
