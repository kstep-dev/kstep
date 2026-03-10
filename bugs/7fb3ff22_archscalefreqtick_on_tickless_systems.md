# sched/core: Fix arch_scale_freq_tick() on tickless systems

- **Commit:** 7fb3ff22ad8772bbf0e3ce1ef3eb7b09f431807f
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

During long tickless periods, the frequency invariance calculations in `arch_scale_freq_tick()` overflow in the `check_shl_overflow()` check. This overflow causes the scheduler to disable the frequency invariance feature globally for all CPUs and emit the warning: "Scheduler frequency invariance went wobbly, disabling!" even though the issue only affects tickless CPUs.

## Root Cause

The `arch_scale_freq_tick()` function is called unconditionally in `scheduler_tick()` for every CPU. On tickless CPUs (those without regular timer interrupts), this function accumulates values without periodic updates over extended periods, causing arithmetic overflow in the shift-and-check operation. The overflow triggers an error condition that disables frequency invariance globally instead of just skipping it for tickless CPUs.

## Fix Summary

The fix adds a check using `housekeeping_cpu(cpu, HK_TYPE_TICK)` before calling `arch_scale_freq_tick()`. This ensures the frequency invariance calculations only run on CPUs with regular ticks, preventing overflow accumulation on tickless CPUs while preserving the feature for CPUs that can maintain accurate frequency tracking.

## Triggering Conditions

The bug is triggered when:
- System enters tickless mode on one or more CPUs (nohz_full or isolated CPUs)
- `scheduler_tick()` continues running on housekeeping CPUs but is infrequent on tickless CPUs
- During long periods without ticks on tickless CPUs, `arch_scale_freq_tick()` accumulates frequency counters
- The accumulated counter values eventually cause overflow in `check_shl_overflow(acnt, 2*SCHED_CAPACITY_SHIFT, &acnt)`
- This overflow triggers the global error handler that disables frequency invariance for all CPUs
- The race requires sufficient elapsed time between ticks on tickless CPUs to cause arithmetic overflow
- Specific CPU topology with both housekeeping and tickless CPUs is needed

## Reproduce Strategy (kSTEP)

To reproduce this bug using kSTEP:
- Configure multi-CPU system (at least 3 CPUs: driver on CPU0, housekeeping on CPU1, tickless on CPU2+)
- Use `kstep_sysctl_write("kernel.nohz_full", "2-3")` to enable tickless mode on CPUs 2-3
- Create tasks pinned to different CPU types: `kstep_task_pin(task_hk, 1, 1)` and `kstep_task_pin(task_tl, 2, 2)`
- In setup(), enable frequency scaling and ensure `arch_scale_freq_tick()` is functional
- Use `kstep_tick_repeat()` with large tick counts to simulate long periods between ticks on tickless CPUs
- Monitor for overflow conditions in `on_tick_begin()` callback by checking frequency invariance state
- Detect bug by watching for "went wobbly" warning or checking if frequency invariance gets disabled globally
- Compare behavior: frequent ticks should work, while simulated long tickless periods should trigger overflow
