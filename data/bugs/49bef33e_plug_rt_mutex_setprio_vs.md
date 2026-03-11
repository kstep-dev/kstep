# sched/rt: Plug rt_mutex_setprio() vs push_rt_task() race

- **Commit:** 49bef33e4b87b743495627a529029156c6e09530
- **Affected file(s):** kernel/sched/rt.c, kernel/sched/deadline.c
- **Subsystem:** RT (Real-Time)

## Bug Description

When a task is demoted from RT to CFS priority while releasing an rt_mutex, a race condition can occur where push_rt_task() attempts to invoke find_lowest_rq() on a non-RT task. This causes incorrect behavior down convert_prio() since that function is designed only for RT tasks. The bug is triggered when the local CPU receives rto_push_work irqwork before the scheduler has a chance to reschedule after the demotion.

## Root Cause

The race occurs because switched_from_rt() may not remove the CPU from the rto_mask when rt_nr_running becomes zero due to a task being demoted. Subsequently, another CPU's rto_push_irq_work_func() can queue rto_push_work on this CPU, causing push_rt_task() to execute while rq->curr is now a CFS task rather than an RT task. The function then passes this non-RT task to find_lowest_rq(), which expects RT-specific task properties.

## Fix Summary

The fix reorders priority and class checks in push_rt_task() to occur before any migration-disabled handling or find_lowest_rq() calls. An explicit check ensures rq->curr is an RT task (sched_class == &rt_sched_class) before calling find_lowest_rq(rq->curr). The same logic is applied to push_dl_task() for consistency.

## Triggering Conditions

- **RT mutex contention**: A task holding an rt_mutex must be demoted from RT to CFS when releasing the mutex via rt_mutex_setprio()
- **Multi-CPU RT load balancing**: At least 2 RT tasks must be migratory and CPU has rt_nr_migratory >= 2 to stay in rto_mask after demotion
- **Race timing**: Another CPU's rto_push_irq_work_func() must queue rto_push_work on the local CPU before the demoted task gets rescheduled
- **Current task state**: The rq->curr must be the demoted CFS task when push_rt_task() executes, attempting to call find_lowest_rq() on non-RT task
- **RT load balancing active**: The RT push/pull mechanism must be active with CPUs in the rto_mask for cross-CPU push work to be queued

## Reproduce Strategy (kSTEP)

- **Setup**: Use at least 3 CPUs (CPU 0 reserved for driver). Create multiple RT tasks with different affinities to trigger RT load balancing
- **RT mutex scenario**: Use kstep_task_create() to create an RT task that will acquire/release an rt_mutex, simulating rt_mutex_setprio() demotion path
- **Multi-CPU RT tasks**: Create 2+ migratory RT tasks with kstep_task_fifo(), ensure rt_nr_migratory >= 2 to keep CPU in rto_mask
- **Timing control**: Use kstep_tick() and on_sched_softirq_begin/end callbacks to precisely control when push_rt_task() executes relative to task demotion
- **Race trigger**: Monitor rq->curr sched_class in callbacks and detect when push_rt_task() tries to call find_lowest_rq() on CFS task
- **Detection**: Check if find_lowest_rq() is called with non-RT task using kernel logging or invariants, look for convert_prio() mayhem symptoms
- **Validation**: Verify the bug by observing push_rt_task() attempting RT operations on CFS current task before explicit class check
