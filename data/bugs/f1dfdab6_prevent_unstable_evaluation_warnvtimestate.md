# sched/vtime: Prevent unstable evaluation of WARN(vtime->state)

- **Commit:** f1dfdab694eb3838ac26f4b73695929c07d92a33
- **Affected file(s):** kernel/sched/cputime.c
- **Subsystem:** core

## Bug Description

The vtime state field is accessed under loose seqcount protection in kcpustat code paths. If `vtime->state` is read multiple times without synchronization, concurrent context switches can cause the state to change between reads, leading to inconsistent logic decisions and potentially triggering false or misleading WARN assertions. This creates a window where conditional checks based on vtime->state can evaluate differently on each read, violating code invariants.

## Root Cause

The `vtime_state_check()` function and its callers read `vtime->state` multiple times across different code paths while only holding loose seqcount protection. Under high contention or during context switches, the vtime->state field can be modified concurrently, causing subsequent reads in the same protected section to return different values. This violates the implicit assumption that vtime->state remains stable within a given seqcount-protected section, leading to inconsistent branch decisions and invalid WARN conditions.

## Fix Summary

The fix captures `vtime->state` once using `READ_ONCE()` at the start of the critical section, then uses the cached local variable throughout the function. The renamed `vtime_state_fetch()` now returns the state value itself, and all callers use this single snapshot to make all subsequent decisions, ensuring state consistency within each seqcount-protected section.

## Triggering Conditions

The bug requires concurrent access to vtime state during seqcount-protected kcpustat operations. It occurs when:
- vtime accounting is enabled and active on a CPU
- kcpustat readers call `vtime_state_check()` during high context switch frequency
- Context switches cause `vtime->state` transitions (e.g., VTIME_SYS ↔ VTIME_USER ↔ VTIME_INACTIVE) 
- Multiple reads of `vtime->state` within the same seqcount section observe different values
- The inconsistent state readings violate conditional logic assumptions, potentially triggering false WARNs

The race window occurs between the initial state check and subsequent state-dependent operations within the seqcount-protected section. High CPU load with frequent task switches maximizes the likelihood of this race condition.

## Reproduce Strategy (kSTEP)

Create high context switch pressure to trigger vtime state races:
- Use 2+ CPUs (CPU 0 reserved for driver)
- Enable vtime accounting via kernel config or runtime parameters
- In `setup()`: Create multiple tasks with `kstep_task_create()` and configure them for rapid scheduling
- In `run()`: Use `kstep_task_pin()` to pin tasks to specific CPUs, then repeatedly call `kstep_task_pause()`/`kstep_task_wakeup()` in tight loops
- Use `kstep_tick_repeat()` with small intervals to force frequent scheduler ticks
- Add `on_tick_begin()` callback to monitor vtime state transitions and detect inconsistent reads
- Log vtime state values before/after operations to catch cases where state changes mid-operation
- Trigger the race by having one task read kcpustat while others rapidly context switch
