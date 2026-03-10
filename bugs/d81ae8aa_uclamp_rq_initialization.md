# sched/uclamp: Fix initialization of struct uclamp_rq

- **Commit:** d81ae8aac85ca2e307d273f6dc7863a721bf054e
- **Affected file(s):** kernel/sched/core.c
- **Subsystem:** uclamp (utilization clamping)

## Bug Description

The `struct uclamp_rq` was initialized by zeroing out all bytes via `memset()`, with the assumption that subsequent calls to `uclamp_rq_inc()` would properly initialize the values. However, when a static key is introduced to skip `uclamp_rq_{inc,dec}()` calls until userspace explicitly enables uclamp, this lazy initialization never occurs. As a result, `rq->uclamp[UCLAMP_MAX].value` remains at 0, causing all runqueues to be capped to 0 by default, which prevents schedutil from performing any CPU frequency changes.

## Root Cause

The initialization code relied on lazy initialization in `uclamp_rq_inc()` to set the proper default values for clamp buckets. When a static key conditionally disables uclamp operations until explicitly enabled by userspace, the lazy initialization path is never executed, leaving the clamp values at 0 (from `memset()`). This violates the invariant that `uclamp[UCLAMP_MAX].value` should default to the maximum allowed value, not 0.

## Fix Summary

The fix introduces a new `init_uclamp_rq()` function that properly initializes each clamp value to `uclamp_none(clamp_id)` (the correct default) and the uclamp_flags to 0. The `init_uclamp()` function is updated to call `init_uclamp_rq()` for each CPU instead of using `memset()`, ensuring proper initialization happens at boot time regardless of whether userspace has enabled uclamp or not.

## Triggering Conditions

The bug occurs during early kernel boot when the uclamp subsystem is compiled in but a static key conditionally disables uclamp operations until userspace explicitly enables them. The `memset()` initialization in `init_uclamp()` zeros out all `struct uclamp_rq` fields, including `rq->uclamp[UCLAMP_MAX].value`. Without the lazy initialization path through `uclamp_rq_inc()` (which is skipped due to the static key), `uclamp[UCLAMP_MAX].value` remains 0 instead of the proper default (`SCHED_CAPACITY_SCALE` for UCLAMP_MAX). This incorrect initialization affects all CPUs immediately at boot and persists until userspace enables uclamp, causing schedutil governor to be capped at 0 frequency scaling.

## Reproduce Strategy (kSTEP)

1. **Setup**: Use a single CPU (CPU 1, since CPU 0 is reserved) with the buggy kernel version that has the static key patch but not this fix.
2. **Observation**: In `setup()`, directly access `cpu_rq(1)->uclamp[UCLAMP_MAX].value` and log its value. On buggy kernels, this should be 0 instead of the expected default.
3. **Frequency scaling test**: Create a simple CFS task with `kstep_task_create()` and run it with `kstep_tick_repeat()`. Use callback hooks to monitor any frequency scaling decisions or changes.
4. **Detection**: The bug is triggered when `rq->uclamp[UCLAMP_MAX].value == 0` at boot time, which can be verified by directly reading the runqueue structure. The impact manifests as schedutil being unable to scale frequency above the minimum.
5. **Validation**: Compare the same test on the fixed kernel where `rq->uclamp[UCLAMP_MAX].value` should equal `SCHED_CAPACITY_SCALE` (1024).
