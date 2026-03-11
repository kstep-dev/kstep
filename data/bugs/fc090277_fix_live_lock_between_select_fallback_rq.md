# sched/rt: Fix live lock between select_fallback_rq() and RT push

- **Commit:** fc09027786c900368de98d03d40af058bcb01ad9
- **Affected file(s):** kernel/sched/cpupri.c
- **Subsystem:** RT

## Bug Description

A live lock occurs when one CPU has an RT task running while another CPU is being offlined and also has an RT task. During offlining, the migration thread repeatedly attempts to migrate the actively running task on the offlining CPU, but select_fallback_rq() keeps picking the same CPU with the running RT task, only to get the task pushed back to the CPU being offlined. This creates an infinite scheduling loop that can hang the machine, observed during RCU-boost testing with the TREE03 rcutorture configuration.

## Root Cause

The select_fallback_rq() function in the RT scheduler's CPU priority picker (cpupri.c) does not check whether candidate CPUs are active for scheduling (in cpu_active_mask) when finding a CPU to place an RT task. When a CPU is being offlined, it is removed from cpu_active_mask but may still have tasks running, leading to the scheduler repeatedly selecting it as a fallback destination, causing the task to be pushed back and forth between the offlining CPU and the migration thread.

## Fix Summary

The fix adds a single line to filter the lowest_mask through cpu_active_mask after finding candidate CPUs. This ensures that only CPUs that are actively scheduled (not being offlined) are selected as destinations for RT task placement, preventing the live lock scenario.

## Triggering Conditions

The bug requires at least two CPUs with RT tasks where one CPU is being taken offline. The RT scheduler's `__cpupri_find()` function in `cpupri.c` fails to check `cpu_active_mask` when selecting fallback CPUs for task placement. When a CPU goes offline, it gets removed from `cpu_active_mask` but may still have RT tasks running. The migration thread attempting to move tasks from the offlining CPU hits `select_fallback_rq()`, which picks the same offlining CPU as a destination, causing the RT push mechanism to bounce the task back. This creates an infinite migration loop between the migration thread and RT push logic, resulting in a live lock that can hang the machine.

## Reproduce Strategy (kSTEP)

Use at least 3 CPUs (CPU 0 reserved for driver). In `setup()`, create two RT tasks using `kstep_task_create()` and `kstep_task_fifo()`. In `run()`, pin one RT task to CPU 1 and another to CPU 2 with `kstep_task_pin()`, then wake them with `kstep_task_wakeup()`. Run `kstep_tick_repeat()` to establish running state. Simulate CPU offlining by manipulating `cpu_active_mask` or using CPU hotplug interfaces through `kstep_write()`. Monitor task migrations with `on_sched_softirq_end()` callback to detect repeated migration attempts. Use `kstep_output_curr_task()` and custom logging to track task bouncing between CPUs. The bug manifests as excessive migration activity and potential system hang when the live lock occurs.
