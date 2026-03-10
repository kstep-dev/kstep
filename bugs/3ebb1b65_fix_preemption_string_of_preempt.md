# Fix preemption string of preempt_dynamic_none

- **Commit:** 3ebb1b6522392f64902b4e96954e35927354aa27
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** core

## Bug Description

The preempt_model_str() function has an off-by-one error in its comparison logic that prevents it from correctly handling the preempt_dynamic_mode value of 0, which represents the "preempt_dynamic_none" mode. When preempt_dynamic_mode equals 0, the function incorrectly returns "PREEMPT(undef)" instead of the correct "PREEMPT(none)", causing the preemption model string output to report an undefined state rather than the valid "none" mode.

## Root Cause

The function uses the comparison `preempt_dynamic_mode > 0` to determine if the mode value is valid for indexing into the preempt_modes array. However, 0 is a legitimate and valid value representing the "none" preemption mode. The greater-than comparison excludes 0, causing the code to treat a valid mode value as invalid and fall through to the "undef" fallback case.

## Fix Summary

Change the comparison operator from `>` to `>=` so that a preempt_dynamic_mode value of 0 is correctly recognized as valid and properly indexed into the preempt_modes array, allowing "preempt_dynamic_none" to be formatted as "PREEMPT(none)" instead of "PREEMPT(undef)".

## Triggering Conditions

- Kernel must be built with `CONFIG_PREEMPT_DYNAMIC=y` and `CONFIG_PREEMPT_BUILD=y`
- The `preempt_dynamic_mode` variable must be set to 0 (representing "preempt_dynamic_none")
- A call to `preempt_model_str()` function must occur (triggered via /proc/version read, kernel boot messages, or direct function call)
- The off-by-one comparison `preempt_dynamic_mode > 0` incorrectly treats the valid mode value 0 as invalid
- Results in the function returning "PREEMPT(undef)" instead of "PREEMPT(none)" in the preemption model string

## Reproduce Strategy (kSTEP)

- Single CPU setup (only CPU 0 needed since this is a display bug, not scheduling logic)
- In setup(): No special task or cgroup configuration required
- In run(): Use kSTEP's kernel symbol access to directly call `preempt_model_str()` or read from `/proc/version`
- Check if the returned string contains "PREEMPT(undef)" when preempt_dynamic_mode is 0 vs "PREEMPT(none)" after fix
- Use `kstep_write()` to access `/proc/version` or import the `preempt_model_str` symbol to call directly
- Log the actual string output with `TRACE_INFO()` for comparison
- Use `kstep_fail()` if "undef" is found in output when mode is 0, `kstep_pass()` if "none" is correctly displayed
