# sched: Fix race in task_call_func()

- **Commit:** 91dabf33ae5df271da63e87ad7833e5fdb4a44b9
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

A race condition exists between `__schedule()` and `task_call_func()` when a task is being deactivated. The task can be dequeued (`on_rq = 0`) but still actively executing on the CPU during context switch. In this narrow window, `task_call_func()` incorrectly determines it doesn't need the runqueue lock, causing lock ordering violations and assertion failures in code like `__set_task_frozen()`.

## Root Cause

The original code checked only `p->on_rq` to determine if the runqueue lock was needed, but failed to synchronize with the complete transition of the task off the CPU. Between when a task is dequeued and when it finally stops executing on the CPU (clears `on_cpu`), the state is inconsistent. `task_call_func()` observes `on_rq == 0` and skips acquiring the runqueue lock, while the scheduler still holds it—causing a lock ordering violation.

## Fix Summary

The fix introduces `__task_needs_rq_lock()` helper function that adds an additional synchronization point: after checking task state and `on_rq`, it uses `smp_cond_load_acquire(&p->on_cpu, !VAL)` to wait for `p->on_cpu` to be cleared. This ensures the task has fully completed execution on the old CPU and the scheduler has finished with it before determining if the runqueue lock is needed.

## Triggering Conditions

The race occurs in the scheduler core during context switch in `__schedule()` when a task transitions from running to sleeping/blocked state. The critical window exists between when `deactivate_task()` sets `p->on_rq = 0` and when the context switch completes and clears `p->on_cpu`. During this narrow window, a concurrent `task_call_func()` call (e.g., from freezer via `__set_task_frozen()`) on another CPU observes the dequeued task and incorrectly skips acquiring the runqueue lock. This requires:
- SMP system with at least 2 CPUs
- Task sleeping/blocking to trigger deactivation path in `__schedule()`  
- Concurrent `task_call_func()` call during the on_rq=0, on_cpu=1 window
- Timing-sensitive race where task_call_func() checks state before context switch completes

## Reproduce Strategy (kSTEP)

Create a multi-CPU setup (minimum 3 CPUs: CPU0 for driver, CPU1 for victim task, CPU2 for task_call_func() caller). Use `kstep_task_create()` to create 2 tasks - one victim task that will sleep/block and trigger deactivation, and one helper task to generate concurrent task_call_func() calls. In `setup()`, pin victim task to CPU1 and helper to CPU2 with `kstep_task_pin()`. In `run()`, use `kstep_task_wakeup()` to start both tasks, then repeatedly call `kstep_task_pause()` and `kstep_task_wakeup()` on the victim to trigger sleep/wake cycles and create context switch opportunities.

Use `on_tick_begin()` callback to log task states (`p->on_rq`, `p->on_cpu`) and detect the race window. Instrument the helper task to repeatedly attempt operations that call `task_call_func()` (like freezer operations). The bug manifests as lock ordering violations or assertion failures when `task_call_func()` incorrectly skips runqueue lock acquisition. Monitor for inconsistent lock states or scheduling anomalies during the narrow race window to confirm reproduction.
