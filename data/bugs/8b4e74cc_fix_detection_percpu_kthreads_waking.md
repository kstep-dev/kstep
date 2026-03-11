# sched/fair: Fix detection of per-CPU kthreads waking a task

- **Commit:** 8b4e74ccb582797f6f0b0a50372ebd9fd2372a27
- **Affected file(s):** kernel/sched/fair.c
- **Subsystem:** CFS (fair scheduling)

## Bug Description

The `select_idle_sibling()` function incorrectly handles the case where a task wakes up from interrupt context while a per-CPU kthread is running. The code assumes any wake-up by a per-CPU kthread should return the previous CPU, but this check is missing a context verification. When a task is woken by an interrupt (e.g., hrtimer) while per-CPU kthreads or idle tasks are running, the incomplete condition spuriously triggers this special case, causing incorrect CPU selection for the woken task. This leads to suboptimal task placement decisions.

## Root Cause

The condition checking for per-CPU kthreads waking a task only verified `is_per_cpu_kthread(current)` but did not verify that the wakeup actually occurred within task context. This allows interrupt context wake-ups (where `current` might be the idle task or a kthread, but the actual work is being done from an interrupt) to be incorrectly treated as per-CPU kthread wake-ups. The fix adds `in_task()` check to ensure the wake-up truly originates from task context.

## Fix Summary

The fix adds an `in_task()` check to the per-CPU kthread wake-up condition, ensuring that only actual task context wake-ups from per-CPU kthreads trigger the special CPU selection logic. This prevents interrupt context wake-ups from being mishandled as per-CPU kthread wake-ups.

## Triggering Conditions

The bug occurs in `select_idle_sibling()` when a task wake-up from interrupt context (e.g., hrtimer) happens while a per-CPU kthread is running. The specific conditions are:
- A per-CPU kthread is currently running (`is_per_cpu_kthread(current)` is true)
- Wake-up occurs from interrupt context (not task context)
- The woken task's previous CPU matches the current CPU (`prev == smp_processor_id()`)
- The current runqueue has ≤ 1 running task (`this_rq()->nr_running <= 1`)
- The missing `in_task()` check allows interrupt context to spuriously trigger the per-CPU kthread optimization path

This causes the scheduler to incorrectly return `prev` CPU instead of performing proper idle sibling selection, leading to suboptimal task placement.

## Reproduce Strategy (kSTEP)

Use 2+ CPUs (CPU 0 reserved for driver). Create a per-CPU kthread on CPU 1 and a regular task that will sleep/wake via timer:
- **Setup**: Create per-CPU kthread with `kstep_kthread_create()` and bind to CPU 1 with `kstep_kthread_bind()`, create regular task with `kstep_task_create()` 
- **Run sequence**: Start kthread with `kstep_kthread_start()`, pin regular task to CPU 1, use `kstep_task_usleep()` to make task sleep and wake via hrtimer
- **Timing**: Use `kstep_tick()` to advance scheduler state while kthread is running when task wake-up occurs
- **Detection**: Hook `on_sched_balance_begin()` to log CPU selection in `select_idle_sibling()`, compare selected CPU between buggy and fixed kernels
- **Verification**: On buggy kernel, task should incorrectly return to previous CPU; on fixed kernel, proper idle sibling selection should occur
