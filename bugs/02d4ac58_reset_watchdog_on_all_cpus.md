# sched/debug: Reset watchdog on all CPUs while processing sysrq-t

- **Commit:** 02d4ac5885a18d326b500b94808f0956dcce2832
- **Affected file(s):** kernel/sched/debug.c
- **Subsystem:** sched/debug

## Bug Description

When processing the sysrq-t command (which dumps scheduler debug information) on a slow serial console with many processes and CPUs, the verbose output can take a long time to complete. During this lengthy output operation, the NMI-watchdog and softlockup watchdog timers can expire and trigger false "lockup detected" messages, even though the system is not actually deadlocked but rather busy generating debug output. These spurious lockup warnings are confusing to users and can obscure real lockup issues.

## Root Cause

The sysrq_sched_debug_show() function iterates through all online CPUs and calls print_cpu() to output scheduler information for each, but does not reset the watchdog timers during this iteration. On slow consoles or systems with many CPUs, this loop can run for a long time without resetting watchdog timers, causing them to expire and generate false lockup warnings. Additionally, other CPUs might be blocked waiting for IPI or stop_machine operations while the debug output is being processed, and they need their softlockup watchdogs reset to prevent false alarms.

## Fix Summary

The fix adds watchdog reset calls inside the CPU iteration loop: touch_nmi_watchdog() resets the NMI watchdog, and touch_all_softlockup_watchdogs() resets softlockup watchdogs on all CPUs. These are called for each CPU iteration during debug output, ensuring watchdog timers are periodically reset throughout the potentially long sysrq-t operation to prevent false lockup messages.

## Triggering Conditions

The bug occurs in the scheduler debug subsystem (kernel/sched/debug.c) when sysrq_sched_debug_show() executes a lengthy for_each_online_cpu() loop calling print_cpu() for each CPU. The triggering conditions require:
- Multiple online CPUs (increasing iteration time proportionally) 
- Slow console output device (serial console) that delays print_cpu() execution
- Many running processes/tasks per CPU (increasing debug info volume per CPU)
- Total debug output time exceeding NMI watchdog threshold (~10s) or softlockup threshold (~20s)
- No watchdog reset calls during the CPU iteration, allowing timers to expire
- Other CPUs potentially blocked on IPIs or stop_machine operations while debug output proceeds

## Reproduce Strategy (kSTEP)

Requires at least 4 CPUs (CPU 0 reserved for driver). In setup(), create multiple tasks using kstep_task_create() and distribute them across CPUs 1-4 using kstep_task_pin() to ensure substantial per-CPU debug information. Use kstep_topo_init() and kstep_topo_apply() to configure multi-CPU topology.

In run(), populate CPUs with running tasks via kstep_task_wakeup() and kstep_tick_repeat(100) to establish scheduler state. Then trigger the debug path by directly calling sysrq_sched_debug_show() (requires kernel symbol import). Use on_tick_begin() callback to monitor watchdog timer state before/after the debug call.

Detection involves checking if NMI watchdog or softlockup warnings appear in kernel logs during debug output, or monitoring watchdog timer expiration counters. The bug manifests as spurious "lockup detected" messages during legitimate debug operations rather than functional scheduler behavior changes.
