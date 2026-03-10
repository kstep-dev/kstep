# sched_ext: Make watchdog handle ops.dispatch() looping stall

- **Commit:** 0922f54fdd15aedb93730eb8cfa0c069cbad4e08
- **Affected file(s):** kernel/sched/ext.c
- **Subsystem:** EXT (sched_ext)

## Bug Description

The dispatch path in sched_ext retries if the local DSQ is still empty after ops.dispatch() either dispatches or consumes a task. A malicious or buggy ops.dispatch() implementation can trap the system by repeatedly dispatching ineligible tasks, causing an infinite dispatch loop that stalls all CPUs. When all CPUs are stalled this way, the watchdog timer and sysrq handler cannot run, making the system unrecoverable and unable to be saved.

## Root Cause

The dispatch loop in balance_scx() lacks an iteration limit. It continues looping as long as ops.dispatch() makes forward progress (dispatches or consumes tasks), with no mechanism to break out and allow other critical kernel code (watchdog, sysrq) to run. This allows a buggy or adversarial ops.dispatch() to stall the CPU indefinitely by repeatedly dispatching ineligible tasks that get dequeued.

## Fix Summary

The fix introduces a loop iteration limit (SCX_DSP_MAX_LOOPS = 32) in the dispatch path. After 32 iterations, the dispatch loop breaks and calls scx_bpf_kick_cpu() to trigger a reschedule on the next task (likely idle), which will eventually lead back to the dispatch path if needed while allowing the watchdog and other critical code to run.

## Triggering Conditions

The bug requires a sched_ext BPF scheduler to be loaded with a malicious or buggy `ops.dispatch()` callback that:
- Repeatedly dispatches tasks that are ineligible for scheduling (e.g., tasks that will be immediately dequeued)
- Ensures the dispatch loop makes forward progress by dispatching or consuming at least one task per iteration
- Affects all CPUs simultaneously to prevent the watchdog timer and sysrq handler from running
- Maintains this behavior long enough to exhaust system responsiveness before any timeout mechanisms can intervene
- The vulnerable code path is `balance_scx()` in `kernel/sched/ext.c` where the dispatch loop lacks iteration bounds

## Reproduce Strategy (kSTEP)

Since this bug involves sched_ext (BPF-based scheduling), direct reproduction with kSTEP is challenging as kSTEP primarily tests the core scheduler. However, a conceptual approach would be:
- Requires multiple CPUs (at least 2: CPU 0 reserved for driver, CPU 1+ for testing)
- In `setup()`: Use `kstep_topo_init()` and `kstep_topo_apply()` to configure a multi-CPU topology
- Create multiple tasks with `kstep_task_create()` and spread them across CPUs with `kstep_task_pin()`
- Use `on_tick_begin()` callback to monitor dispatch loop iterations and CPU stall detection
- Detection: Monitor for system unresponsiveness, lack of tick progress, or inability to schedule new tasks
- The test would verify whether the system can recover from a stalled dispatch loop scenario
- Note: Full reproduction requires implementing a malicious sched_ext BPF program, which is outside kSTEP's core scheduler testing scope
