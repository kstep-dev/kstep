# Cpufreq: Superfluous Frequency Updates Due to need_freq_update Never Being Cleared

**Commit:** `8e461a1cb43d69d2fc8a97e61916dce571e6bb31`
**Affected files:** kernel/sched/cpufreq_schedutil.c
**Fixed in:** v6.14-rc1
**Buggy since:** v5.3-rc5 (introduced by commit `600f5badb78c` "cpufreq: schedutil: Don't skip freq update when limits change")

## Bug Description

The schedutil cpufreq governor performs unnecessary (superfluous) frequency update operations because the `need_freq_update` flag in `struct sugov_policy` is never properly cleared under certain conditions. This bug manifests in two distinct ways depending on the cpufreq driver in use and the workload characteristics.

First, for cpufreq drivers that set the `CPUFREQ_NEED_UPDATE_LIMITS` flag, the `need_freq_update` flag is effectively permanently set to `true` once any policy limits change occurs. This is because, in the buggy code, `sugov_update_next_freq()` re-tests `cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS)` every time `need_freq_update` is true, and since the driver flag is a static property, it always evaluates to `true`, preventing the flag from ever being cleared. This causes every subsequent frequency decision — even when the computed frequency is identical to the current one — to bypass the "same frequency" early-return optimization in `sugov_update_next_freq()`, resulting in redundant calls to `cpufreq_driver_fast_switch()` or queueing of unnecessary irq_work for slow-switch platforms.

Second, the `ignore_dl_rate_limit()` function sets `limits_changed = true` whenever SCHED_DEADLINE bandwidth increases, which in turn sets `need_freq_update = true` unconditionally in `sugov_should_update_freq()`. This occurs regardless of whether the cpufreq driver specifies `CPUFREQ_NEED_UPDATE_LIMITS`. Combined with the clearing bug described above, this leads to redundant frequency updates triggered by DL bandwidth changes, even when the next computed frequency is the same as the current one.

The net effect is wasted CPU cycles spent performing frequency transitions (or attempting to) when none are actually needed. On slow-switch platforms, this means unnecessary irq_work and kthread scheduling; on fast-switch platforms, it means unnecessary cross-CPU IPI-like frequency switch calls. While the functional frequency outcome is correct (the right frequency is chosen), the overhead of the superfluous updates degrades both performance and energy efficiency.

## Root Cause

The root cause lies in the interaction between three functions in `cpufreq_schedutil.c`: `sugov_should_update_freq()`, `sugov_update_next_freq()`, and `ignore_dl_rate_limit()`.

**The `limits_changed` → `need_freq_update` propagation problem:** In `sugov_should_update_freq()`, when `sg_policy->limits_changed` is true (set by `sugov_limits()` on policy limit changes or by `ignore_dl_rate_limit()` on DL bandwidth increases), the buggy code unconditionally sets `need_freq_update = true`:

```c
if (unlikely(sg_policy->limits_changed)) {
    sg_policy->limits_changed = false;
    sg_policy->need_freq_update = true;  // BUG: always true, even for non-NEED_UPDATE_LIMITS drivers
    return true;
}
```

The intent of `need_freq_update` is to force `get_next_freq()` to call `cpufreq_driver_resolve_freq()` (bypassing the `cached_raw_freq` optimization) and to force `sugov_update_next_freq()` to proceed even when the next frequency equals the current one. This is only truly needed for drivers with `CPUFREQ_NEED_UPDATE_LIMITS`, where the driver may need to be poked to re-apply internal hardware limits even at the same frequency.

**The never-clearing problem in `sugov_update_next_freq()`:** The buggy `sugov_update_next_freq()` attempts to clear `need_freq_update` but does so incorrectly:

```c
if (sg_policy->need_freq_update)
    sg_policy->need_freq_update = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);
else if (sg_policy->next_freq == next_freq)
    return false;
```

When `need_freq_update` is true and the driver has `CPUFREQ_NEED_UPDATE_LIMITS`, the flag is reassigned `true` (since the driver flag is a static property of the driver). The flag is never reset to `false`. This creates a permanent latch: once set, `need_freq_update` stays true forever for `CPUFREQ_NEED_UPDATE_LIMITS` drivers, defeating the "same frequency" early-return optimization and causing every frequency calculation to proceed to a full update.

**The `ignore_dl_rate_limit()` amplification:** The `ignore_dl_rate_limit()` function further exacerbates the issue. It sets `limits_changed = true` every time DL bandwidth increases, which feeds back into `sugov_should_update_freq()` setting `need_freq_update = true`. For non-`CPUFREQ_NEED_UPDATE_LIMITS` drivers, this means a DL bandwidth increase triggers a redundant frequency update even when the next chosen frequency is the same as the current one, because `need_freq_update` bypasses the frequency comparison check. While `sugov_update_next_freq()` does clear `need_freq_update` for these drivers (since `cpufreq_driver_test_flags()` returns false), the redundant update still happens once per DL bandwidth increase event.

**The initialization problem:** At `sugov_start()`, `need_freq_update` is initialized to `cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS)`, meaning for `CPUFREQ_NEED_UPDATE_LIMITS` drivers, the superfluous update behavior starts from the very first frequency calculation, not just after a limits change.

## Consequence

The primary consequence is unnecessary CPU overhead from superfluous frequency transitions. For cpufreq drivers with `CPUFREQ_NEED_UPDATE_LIMITS` (such as `intel_pstate` in passive mode), every scheduler tick or utilization update that triggers schedutil results in a full frequency update operation — including calling `cpufreq_driver_resolve_freq()` and then `cpufreq_driver_fast_switch()` (or queueing irq_work for slow-switch) — even when the frequency hasn't changed. On systems with high tick rates or many CPUs, this can result in thousands of unnecessary frequency switch operations per second.

For fast-switch platforms, each superfluous update involves an unnecessary hardware register write or firmware call. For slow-switch platforms, it means unnecessary irq_work queueing and kthread wakeups. In both cases, this wastes energy (additional CPU cycles spent on frequency management rather than useful work) and can increase scheduling latency (especially on slow-switch platforms where the kthread may compete for CPU time). The performance impact scales with the number of CPUs and the frequency of scheduler events.

Additionally, for non-`CPUFREQ_NEED_UPDATE_LIMITS` drivers, DL bandwidth changes trigger unnecessary frequency updates. While this is a less persistent issue (since `need_freq_update` does get cleared for these drivers), it still represents wasted work. The reviewer Christian Loehle confirmed the issue with "Good catch!" in the mailing list review, indicating this was a recognized overhead concern among cpufreq maintainers.

## Fix Summary

The fix restructures where `CPUFREQ_NEED_UPDATE_LIMITS` is tested and how `need_freq_update` is cleared.

**In `sugov_should_update_freq()`**, the fix moves the `CPUFREQ_NEED_UPDATE_LIMITS` test to the point where `need_freq_update` is set:

```c
if (unlikely(sg_policy->limits_changed)) {
    sg_policy->limits_changed = false;
    sg_policy->need_freq_update = cpufreq_driver_test_flags(CPUFREQ_NEED_UPDATE_LIMITS);
    return true;
}
```

This ensures that `need_freq_update` is only set to `true` when the driver actually requires it (i.e., the driver has `CPUFREQ_NEED_UPDATE_LIMITS`). For all other drivers, `need_freq_update` remains `false` after a limits change, which means the `limits_changed` event only bypasses the rate limit (by returning `true`) without also disabling the frequency comparison optimization.

**In `sugov_update_next_freq()`**, the fix unconditionally clears `need_freq_update`:

```c
if (sg_policy->need_freq_update)
    sg_policy->need_freq_update = false;
else if (sg_policy->next_freq == next_freq)
    return false;
```

This ensures that `need_freq_update` is always a one-shot flag: it forces exactly one frequency update to proceed regardless of the frequency comparison, and then it's cleared. For `CPUFREQ_NEED_UPDATE_LIMITS` drivers, this means the flag is set on limits change (from `sugov_should_update_freq()`) and cleared immediately after the forced update completes (in `sugov_update_next_freq()`), rather than persisting forever. The combination of these two changes ensures that the "force update" mechanism works correctly as a single-event trigger rather than a permanent latch.

## Triggering Conditions

To trigger the superfluous update behavior, the following conditions are needed:

1. **A cpufreq driver with `CPUFREQ_NEED_UPDATE_LIMITS`**: The permanent-latch variant of the bug requires a driver that sets this flag (e.g., `intel_pstate` in passive mode). For the DL-related variant, any cpufreq driver will do.

2. **The `schedutil` governor must be active**: The bug is entirely within the schedutil governor code path (`kernel/sched/cpufreq_schedutil.c`). Other governors (ondemand, performance, powersave) are unaffected.

3. **A policy limits change or DL bandwidth increase must occur**: For the permanent-latch variant, at least one `limits_changed` event must happen (e.g., from thermal throttling via `sugov_limits()`, or from `ignore_dl_rate_limit()`). For the DL variant, a SCHED_DEADLINE task must increase its bandwidth allocation on a CPU managed by schedutil.

4. **Subsequent scheduler utilization updates**: After the initial triggering event, every `cpufreq_update_util()` call on the affected CPU (triggered by scheduler ticks, task wakeups, or other scheduling events) will result in a superfluous frequency update. The more frequent the scheduler events, the greater the overhead.

5. **The computed next frequency must equal the current frequency**: The superfluous updates are specifically those where the frequency comparison `sg_policy->next_freq == next_freq` would have returned `false` (preventing a real update), but `need_freq_update` being stuck at `true` bypasses this check. This is most visible during steady-state workloads where the frequency has stabilized.

The bug is deterministic and 100% reproducible given the above conditions. It is not a race condition — it is a logic error in flag management. The bug has existed since Linux v5.3-rc5 (commit `600f5badb78c`), making it a long-standing issue affecting all systems using schedutil with `CPUFREQ_NEED_UPDATE_LIMITS` drivers for over five years.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. The reasons are as follows:

**1. No cpufreq driver in QEMU:** The schedutil governor requires a registered cpufreq driver to function. QEMU's virtualized CPUs do not expose cpufreq hardware (no ACPI P-states, no frequency scaling registers, no firmware frequency control). Without a cpufreq driver, the schedutil governor is never activated, and the entire code path in `cpufreq_schedutil.c` — including `sugov_should_update_freq()`, `sugov_update_next_freq()`, `get_next_freq()`, and all the `sugov_update_*()` callbacks — is never invoked.

**2. No `cpufreq_update_util()` hook registration:** Even if schedutil could theoretically be loaded, the `cpufreq_add_update_util_hook()` call in `sugov_start()` requires a valid `struct cpufreq_policy` backed by a real driver. Without this registration, the scheduler's `cpufreq_update_util()` calls in `scheduler_tick()` and `enqueue_task_fair()` are no-ops (they check for a registered hook per-CPU).

**3. kSTEP's `kstep_cpu_set_freq()` is unrelated:** kSTEP provides `kstep_cpu_set_freq(cpu, scale)` to set `arch_freq_scale`, but this only affects the Frequency Invariance Engine (FIE) scaling factor used by PELT. It does not register a cpufreq driver, activate the schedutil governor, or trigger any of the schedutil code paths where the bug exists.

**4. `need_freq_update` and `limits_changed` are internal to schedutil:** These are fields of `struct sugov_policy`, which is allocated and managed entirely within the schedutil governor. There is no kernel API to externally manipulate these fields or to simulate the `sugov_limits()` callback that sets `limits_changed = true`.

**5. No way to observe the bug's effect:** The bug's consequence is superfluous calls to `cpufreq_driver_fast_switch()` or unnecessary irq_work for `__cpufreq_driver_target()`. These functions interact with real cpufreq driver hardware. Without a driver, there's nothing to call and nothing to observe.

**What would need to be added to kSTEP:** To support reproducing cpufreq governor bugs, kSTEP would need fundamental additions:
- A mock cpufreq driver that registers with the cpufreq framework, providing a `struct cpufreq_policy` with configurable min/max/cur frequencies and driver flags (including `CPUFREQ_NEED_UPDATE_LIMITS`).
- The ability to activate the schedutil governor on the mock driver's policy.
- The ability to trigger `sugov_limits()` (simulating policy limits changes from thermal or userspace).
- Instrumentation hooks within `sugov_update_next_freq()` or `cpufreq_driver_fast_switch()` to count and log frequency update operations.
These changes would be fundamental additions to kSTEP's architecture, not minor extensions, since they require integrating with the cpufreq subsystem's driver registration and governor framework.

**Alternative reproduction outside kSTEP:** The bug can be reproduced on real hardware or in a VM with a cpufreq driver (e.g., `acpi-cpufreq` or `intel_pstate` in passive mode on bare-metal x86):
1. Boot with `cpufreq.default_governor=schedutil`.
2. Add `printk()` tracing in `sugov_update_next_freq()` to log when `need_freq_update` is true and `next_freq == sg_policy->next_freq`.
3. Trigger a limits change (e.g., `echo <freq> > /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq`).
4. Run a steady-state workload. Observe that `need_freq_update` remains permanently true (for `CPUFREQ_NEED_UPDATE_LIMITS` drivers) and frequency updates are issued every tick even when the frequency is unchanged.
5. Alternatively, use `perf` or `ftrace` to trace `cpufreq_driver_fast_switch()` call frequency before and after a limits change to observe the increase in update rate.
