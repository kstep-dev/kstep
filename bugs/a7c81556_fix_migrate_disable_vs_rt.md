# sched: Fix migrate_disable() vs rt/dl balancing

- **Commit:** a7c81556ec4d341dfdbf2cc478ead89d73e474a7
- **Affected file(s):** kernel/sched/core.c, kernel/sched/deadline.c, kernel/sched/rt.c, kernel/sched/sched.h
- **Subsystem:** RT, Deadline, core

## Bug Description

When migrate_disable() is used, all tasks on a system can become stuck on a single CPU, each preempted in a migrate_disable() section by a single high-priority task. This causes lower-priority tasks to be deprived of runtime indefinitely, irrevocably losing system bandwidth. The system cannot approximate running even the M highest-priority tasks as intended.

## Root Cause

The RT/DL balancers (pull_rt_task and pull_dl_task) lack logic to handle the case where they select a lower-priority task to run but that task has migrate_disable set. Without special handling, they simply fail to pull the task, leaving it stuck. The higher-priority task holding migrate_disable remains on the CPU, preventing proper load balancing and starving lower-priority tasks.

## Fix Summary

The fix adds a new `push_cpu_stop()` callback and extends the RT/DL balancers to detect when a task to be pulled has migrate_disable set. Instead of failing to pull, they now push the currently running (higher-priority) task away to another CPU, freeing bandwidth for the lower-priority task. A new MDF_PUSH flag and rq->push_busy mechanism track and coordinate these pushes, ensuring tasks are properly migrated away even when migrate_disable is active.

## Triggering Conditions

The bug occurs when RT/DL tasks use migrate_disable() and trigger load balancing scenarios where:
- A high-priority RT/DL task runs on a CPU with migrate_disable() active
- Lower-priority RT/DL tasks exist on other CPUs that should be pulled for load balancing
- The RT/DL pull balancer (pull_rt_task/pull_dl_task) selects a lower-priority task to migrate but finds migrate_disable() set
- Without the fix, the pull fails and the high-priority task remains stuck, preventing proper load distribution across CPUs
- Multiple tasks can stack on a single CPU in migrate_disable() sections, causing severe bandwidth loss

## Reproduce Strategy (kSTEP)

Requires 3+ CPUs (CPU 0 reserved). Create high-priority RT task on CPU 1 with simulated migrate_disable(), lower-priority RT tasks on CPUs 2-3:
- `setup()`: Create 3 RT tasks, set different priorities via kstep_task_fifo() and kstep_task_set_prio()
- Pin high-priority task to CPU 1, lower-priority tasks to CPUs 2-3 using kstep_task_pin()
- `run()`: Start all tasks with kstep_task_wakeup(), then simulate migrate_disable on high-priority task
- Trigger load balancing via kstep_tick_repeat() to invoke pull_rt_task()
- Use on_sched_balance_begin/selected callbacks to observe pull attempts failing for migrate_disabled task
- Check CPU load distribution with kstep_output_nr_running() - bug manifests as uneven load despite available CPUs
- Verify bandwidth loss by measuring task progress across CPUs before/after balancing attempts
