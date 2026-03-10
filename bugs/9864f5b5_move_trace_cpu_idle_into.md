# cpuidle: Move trace_cpu_idle() into generic code

- **Commit:** 9864f5b5943ab0f1f835f21dc3f9f068d06f5b52
- **Affected file(s):** kernel/sched/idle.c
- **Subsystem:** core

## Bug Description

The trace_cpu_idle() function was being called from architecture-specific implementations using trace_*_rcuidle() variants, which are unsafe tracing functions meant to be called with RCU disabled. This creates a correctness issue where tracing occurs while RCU is disabled, which can lead to inconsistent tracing behavior and potential data loss in trace logs.

## Root Cause

The trace_cpu_idle() calls were embedded in architecture-specific arch_cpu_idle() implementations and invoked with RCU already disabled (via trace_*_rcuidle() macros). This violates the contract that tracing functions should be called with RCU enabled, potentially causing races or missed trace events.

## Fix Summary

The fix moves trace_cpu_idle() calls into the generic default_idle_call() function in kernel/sched/idle.c, placing them strategically before rcu_idle_enter() and after rcu_idle_exit(). This ensures RCU is enabled when tracing occurs, eliminating the need for unsafe trace_*_rcuidle() variants and providing consistent tracing behavior across all architectures.

## Triggering Conditions

The bug manifests when CPU idle tracing is enabled and CPUs enter idle states through architecture-specific idle implementations. The triggering conditions include:
- CONFIG_CPU_IDLE or tracing infrastructure enabled with trace_cpu_idle() events
- CPU becomes idle and calls arch_cpu_idle() implementation (x86, ARM64, s390, etc.)
- Architecture-specific idle code uses trace_cpu_idle_rcuidle() while RCU is disabled
- RCU subsystem is in idle/disabled state during tracing, causing potential data races
- Tracing events may be dropped, corrupted, or cause inconsistent trace buffer state
- The bug occurs in the idle path critical section between rcu_idle_enter() and rcu_idle_exit()

## Reproduce Strategy (kSTEP)

Reproducing this tracing correctness issue requires triggering CPU idle states and observing trace consistency:
- Use 2+ CPUs (CPU 0 reserved for driver, CPU 1+ for idle testing)
- Setup: Create minimal tasks to control CPU idle entry timing
- Use kstep_task_create() to create a single task, then kstep_task_pause() it
- Call kstep_tick_repeat() to advance time and force CPU into idle state
- Monitor idle entry through on_tick_begin/on_tick_end callbacks
- Enable kernel tracing via kstep_sysctl_write("kernel/tracing", "1") if available
- Detect bug by checking trace consistency or RCU state violations during idle
- On buggy kernel: trace events may be lost/corrupted when RCU disabled
- On fixed kernel: trace events should be properly recorded with RCU enabled
