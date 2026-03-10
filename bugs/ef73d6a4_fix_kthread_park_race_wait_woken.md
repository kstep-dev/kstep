# sched/wait: Fix a kthread_park race with wait_woken()

- **Commit:** ef73d6a4ef0b35524125c3cfc6deafc26a0c966a
- **Affected file(s):** kernel/sched/wait.c, kernel/kthread.c, include/linux/kthread.h
- **Subsystem:** core

## Bug Description

A race condition exists between kthread_park() and wait_woken() that prevents a kthread from properly waking up or being parked. When a kthread calls wait_woken() to sleep, and another thread concurrently calls kthread_park(), the kthread may fail to exit the wait and respond to the park request due to insufficient synchronization on the park condition check.

## Root Cause

The wait_woken() function was checking only kthread_should_stop() to determine if a kthread should skip scheduling, but it was not checking kthread_should_park(). This created a race window where kthread_park() could be called while the thread was in wait_woken(), and the thread would proceed to call schedule_timeout() instead of waking up to handle the park request, similar to the race condition that was previously fixed for kthread_stop().

## Fix Summary

The fix introduces a new helper function kthread_should_stop_or_park() that checks both kthread_should_stop() and kthread_should_park() conditions. The wait_woken() function now uses this extended check instead of only checking kthread_should_stop(), ensuring that kthread_park() calls are properly recognized and handled alongside kthread_stop() calls.

## Triggering Conditions

- A kthread that uses `wait_woken()` for sleeping/waiting operations
- The kthread must be in the middle of executing `wait_woken()`, specifically between the `set_current_state()` call and the `schedule_timeout()` call
- Another thread must call `kthread_park()` on the target kthread during this narrow race window
- The timing window occurs when the kthread has set its state but hasn't yet called `schedule_timeout()`
- The race manifests as the kthread proceeding to sleep via `schedule_timeout()` instead of waking up to handle the park request
- The bug requires the kthread to have the `PF_KTHREAD` flag set and be using wait/wake mechanisms that rely on `wait_woken()`

## Reproduce Strategy (kSTEP)

- **CPUs needed**: 2 (CPU 0 reserved for driver, CPU 1 for victim kthread)  
- **Setup**: Use `kstep_kthread_create()` to create a victim kthread that repeatedly calls a wait/wake pattern simulating `wait_woken()` usage
- **Race creation**: Create a timing loop where the driver repeatedly calls the park operation on the victim kthread while it's potentially in `wait_woken()`
- **Detection method**: Monitor kthread state transitions and park/unpark operations using custom logging in the kthread function
- **Key sequence**: 1) Start victim kthread with wait/wake loops, 2) From driver thread, repeatedly call park/unpark operations with tight timing, 3) Check if kthread fails to respond to park requests within expected timeframe
- **Observable behavior**: Log when park requests are issued vs when kthread actually enters parked state - successful race reproduction shows delayed or missed park handling
- **Callback usage**: Use `on_tick_begin()` to log kthread states and park request timing to detect the race condition manifestation
