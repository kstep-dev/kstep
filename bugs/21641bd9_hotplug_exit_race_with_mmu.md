# lazy tlb: fix hotplug exit race with MMU_LAZY_TLB_SHOOTDOWN

- **Commit:** 21641bd9a7a7ce0360106a5a8e5b89a4fc74529d
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core (CPU hotplug, lazy TLB)

## Bug Description

During CPU hotplug/offline, there is a race window where a CPU is cleared from the mm_cpumask of all memory management structures by __cpu_disable(), but the CPU still uses a lazy TLB mm. If a user mm exits during this window (before the CPU switches to init_mm), the user mm will not be subjected to lazy TLB shootdown and may be freed while still actively in use as a lazy mm by the offlining CPU. This results in use-after-free and memory corruption.

## Root Cause

The CPU unplug sequence calls __cpu_disable() (which clears the CPU from mm_cpumask), but the CPU does not switch away from the lazy TLB mm until much later in arch_cpu_idle_dead() when idle_task_exit() is called. This creates a race window where a user mm can be freed due to the mm_cpumask bit being cleared, even though the offlining CPU is still actively using that mm as a lazy TLB context.

## Fix Summary

The fix moves the lazy TLB mm switching from idle_task_exit() to sched_cpu_wait_empty(), which is called much earlier in the CPU hotplug sequence before the CPU teardown. This eliminates the race window by ensuring the CPU switches to init_mm before __cpu_disable() clears the CPU from mm_cpumask structures. The function is renamed from idle_task_exit() to sched_force_init_mm() and uses irq-safe mm switching primitives.

## Triggering Conditions

The bug requires a specific timing window during CPU hotplug operations:
- A CPU must be in the process of being offlined (entering CPU hotplug sequence)
- The CPU hotplug sequence calls __cpu_disable(), clearing the CPU from mm_cpumask of all mms
- The offlining CPU is still using a user mm as lazy TLB context (hasn't switched to init_mm yet)
- A user process exits and its mm is freed during this race window
- The offlining CPU continues using the freed mm as lazy TLB until arch_cpu_idle_dead()
- MMU_LAZY_TLB_SHOOTDOWN must be enabled for the use-after-free to manifest

## Reproduce Strategy (kSTEP)

This bug involves CPU hotplug infrastructure which kSTEP cannot directly simulate. However, we can approximate the race conditions:
- Use 3+ CPUs (CPU 0 reserved for driver, test on CPUs 1-2)
- In setup(): Create tasks that generate user mm contexts on target CPUs
- In run(): Use kstep_task_create() to create processes that will exit, simulating mm lifecycle
- Create callback functions on_tick_begin() to monitor active_mm state changes
- Use kstep_task_pause() followed by kstep_task_wakeup() to simulate mm reference changes
- Monitor mm reference counts and active_mm pointers during task state transitions
- Check for inconsistent mm states where a task's active_mm differs from expected init_mm
- Look for scenarios where mm references persist after task exit (indicating potential UAF)
