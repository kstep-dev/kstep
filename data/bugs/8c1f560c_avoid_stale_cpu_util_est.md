# Avoid stale CPU util_est value for schedutil in task dequeue

- **Commit:** 8c1f560c1ea3f19e22ba356f62680d9d449c9ec2
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (Completely Fair Scheduler)

## Bug Description

During task dequeue, the CPU estimated utilization (util_est) is updated after schedutil has already sampled the CPU utilization for frequency selection. This causes schedutil to use stale util_est values when making frequency decisions, particularly during task ramp-down or ramp-up scenarios. The result is potentially incorrect CPU frequency selection by the frequency governor due to overestimated CPU utilization.

## Root Cause

The util_est update in dequeue_task_fair() occurs in two stages: first the PELT-based util_avg is updated (which triggers schedutil callbacks), and only afterwards is the estimated utilization util_est updated. When schedutil's cpu_util_cfs() function is called during the PELT update, it computes CPU_utilization as max(CPU_util, CPU_util_est), using the old util_est value from before dequeue. This ordering mismatch causes stale estimates to influence frequency scaling decisions.

## Fix Summary

The fix splits the util_est dequeue operation into two separate functions: util_est_dequeue() for updating the root cfs_rq's estimated utilization, and util_est_update() for updating the task's estimated utilization. The util_est_dequeue() call is moved to execute before the load average updates in dequeue_task_fair(), ensuring that when schedutil queries the CPU utilization, it sees the freshly updated util_est value rather than a stale one.

## Triggering Conditions

- Tasks with significant util_est values must be running and then dequeuing
- The schedutil frequency governor must be active (CONFIG_CPU_FREQ_GOV_SCHEDUTIL=y)
- CPU utilization should be driven by util_est rather than util_avg for maximum impact
- Bug manifests during task ramp-down scenarios when task's util_est > util_avg
- Frequency scaling decisions use max(util_avg, util_est), so stale util_est causes overestimation
- Most observable when tasks have been running long enough to build up util_est but then sleep
- The timing window between PELT updates (triggering schedutil) and util_est updates is critical

## Reproduce Strategy (kSTEP)

- Need at least 2 CPUs (CPU 0 reserved for driver, use CPU 1 for test task)
- In setup(): Enable schedutil governor if available, create a high-utilization task
- In run(): Use kstep_task_create() and kstep_task_wakeup() to start a CPU-intensive task
- Run for sufficient time with kstep_tick_repeat(100+) to build up util_est values
- Use kstep_task_pause() to trigger dequeue_task_fair() path
- In on_tick_end(): Log CPU util_avg, util_est values before/after dequeue operations
- Monitor schedutil frequency selection decisions through cpufreq callbacks if available
- Check for cases where CPU util_est remains high during dequeue while util_avg drops
- Successful reproduction shows stale util_est being used in frequency scaling decisions
