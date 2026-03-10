# sched: Fix stop_one_cpu_nowait() vs hotplug

- **Commit:** f0498d2a54e7966ce23cd7c7ff42c64fa0059b07
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/fair.c, kernel/sched/rt.c
- **Subsystem:** Core scheduler, fair scheduling, real-time scheduling, deadline scheduling

## Bug Description

During CPU hotplug operations concurrent with sched_setaffinity(), affine_move_task() can become stuck waiting for completion indefinitely, resulting in hung-task detector warnings. The issue occurs when stop_one_cpu_nowait() returns false (indicating failure to queue work) because the target CPU's stopper is disabled during the hotplug takedown sequence, preventing the migration_cpu_stop() callback from running and completing the pending work.

## Root Cause

A race condition exists between the unlock of the runqueue lock and the call to stop_one_cpu_nowait(). When task_rq_unlock() is called, the stopper thread can preempt and execute the CPU hotplug takedown path, which calls stop_machine_park() and sets stopper->enabled = false. When stop_one_cpu_nowait() is subsequently called, it checks if (stopper->enabled) and returns false without queuing the work, leaving affine_move_task() blocked in wait_for_completion() with no stopper callback to wake it.

## Fix Summary

The fix wraps the unlock and stop_one_cpu_nowait() sequence with preempt_disable()/preempt_enable() calls to prevent the stopper thread from preempting between the lock release and the work queue. This ensures that if the CPU was online when checked under the lock, the stopper will still be enabled when the work is queued, guaranteeing the callback will execute and the completion will be signaled.

## Triggering Conditions

The race requires concurrent CPU hotplug and task affinity changes. Specifically:
- Task calling sched_setaffinity() enters affine_move_task() with runqueue lock held  
- Another CPU initiates hotplug takedown via _cpu_down() → takedown_cpu() → stop_machine_cpuslocked()
- Between task_rq_unlock() and stop_one_cpu_nowait(), the stopper thread preempts on the target CPU
- The preempting stopper executes the hotplug path: take_cpu_down() → __cpu_disable() → stop_machine_park()
- This sets stopper->enabled = false before stop_one_cpu_nowait() executes
- stop_one_cpu_nowait() checks stopper->enabled and returns false, causing indefinite wait_for_completion()
- Timing-sensitive: narrow window between unlock and stop_one_cpu_nowait() call

## Reproduce Strategy (kSTEP)

Set up multi-CPU environment (≥3 CPUs: driver on CPU0, target on CPU1, hotplug on CPU2+):
1. In setup(): Use kstep_topo_init() and kstep_topo_apply() to configure multi-CPU topology
2. Create target task with kstep_task_create() and pin to CPU1 using kstep_task_pin(task, 1, 1)
3. In run(): Trigger sched_setaffinity by calling kstep_task_pin(task, 2, 2) to force migration
4. Concurrently simulate hotplug by writing to /sys/devices/system/cpu/cpu2/online using kstep_write()
5. Use on_tick_begin() callback to monitor task migration state and detect hung affine_move_task()
6. Check for timeout in wait_for_completion() by monitoring task state with kstep_output_curr_task()
7. Log stopper->enabled state and stop_one_cpu_nowait() return values via TRACE_INFO()
8. Success: observe hung task warnings and incomplete migration; Failure: migration completes normally
