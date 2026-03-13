# Cpufreq: CPPC Frequency Invariance kthread/irq Work Race on Policy Exit

**Commit:** `771fac5e26c17845de8c679e6a947a4371e86ffc`
**Affected files:** kernel/sched/core.c (EXPORT_SYMBOL removal), drivers/cpufreq/cppc_cpufreq.c, include/linux/arch_topology.h, drivers/cpufreq/Kconfig.arm
**Fixed in:** v5.13-rc7
**Buggy since:** v5.13-rc1 (introduced by commit 4c38f2df71c8 "cpufreq: CPPC: Add support for frequency invariance")

## Bug Description

The CPPC (Collaborative Processor Performance Control) cpufreq driver added support for frequency invariance in commit 4c38f2df71c8, which was merged for Linux 5.13-rc1. This feature used CPPC delivered and reference performance counters to update the per-CPU `arch_freq_scale` variable, enabling the scheduler to make frequency-invariant scheduling decisions on ARM64 systems with ACPI CPPC firmware.

The frequency invariance implementation used a chain of asynchronous work mechanisms: on every scheduler tick, `cppc_scale_freq_tick()` was called (registered via `topology_set_scale_freq_source()`), which queued an `irq_work` item (since the tick runs in hard-IRQ context and CPPC counter reads may sleep). The `irq_work` handler (`cppc_irq_work()`) then queued a `kthread_work` item onto a global `kworker_fie` kthread worker, and the actual counter read and `arch_freq_scale` update happened in `cppc_scale_freq_workfn()`.

The critical bug was that this work chain was not properly torn down during cpufreq policy exit events — specifically during CPU hotplug (offline) and system suspend/resume. When a CPU was taken offline or the system went to suspend, the cpufreq policy for the affected CPUs would be destroyed, but the pending `irq_work` and `kthread_work` items were not cancelled or synchronized before the associated `cppc_freq_invariance` and `cppc_cpudata` structures became invalid. This resulted in use-after-free accesses and other race conditions.

The fix commit is a full revert of the feature because, as stated by the author Viresh Kumar, "a proper fix won't be possible for the 5.13-rc, as it requires a lot of changes." The feature was later reintroduced with proper lifecycle management in subsequent kernel versions.

## Root Cause

The root cause lies in the missing synchronization of the `irq_work` and `kthread_work` items during cpufreq policy teardown. The per-CPU `cppc_freq_invariance` structures were initialized in `cppc_freq_invariance_policy_init()` when a cpufreq policy was created, and the global `kworker_fie` kthread worker was created in `cppc_freq_invariance_init()` during module init. However, the only cleanup path was `cppc_freq_invariance_exit()` at module unload time, which called `irq_work_sync()` for each CPU and then `kthread_destroy_worker()`.

There was no per-policy cleanup function called during CPU hotplug or suspend/resume. When a CPU went offline, the scheduler tick would stop, but there could be in-flight `irq_work` or `kthread_work` items that had already been queued but not yet executed. Additionally, the `cppc_fi->cpu_data` pointer (set in `cppc_freq_invariance_policy_init()`) pointed to policy-owned `cppc_cpudata` structures that would be freed when the policy was destroyed.

Specifically, the race window was:
1. Scheduler tick fires on a CPU, calling `cppc_scale_freq_tick()`.
2. `irq_work_queue(&cppc_fi->irq_work)` is called, scheduling deferred IRQ work.
3. CPU hotplug begins — the CPU is being taken offline.
4. The cpufreq policy is torn down and `cppc_cpudata` is freed.
5. The `irq_work` fires, calling `cppc_irq_work()`, which queues `kthread_work`.
6. The `kthread_work` fires on `kworker_fie`, calling `cppc_scale_freq_workfn()`.
7. `cppc_scale_freq_workfn()` dereferences `cppc_fi->cpu_data` — which now points to freed memory.

The function `cppc_scale_freq_workfn()` called `cppc_get_perf_ctrs(cppc_fi->cpu, &fb_ctrs)` and then accessed `cpu_data->perf_caps.highest_perf` and `cpu_data->perf_ctrls.desired_perf`, all through the stale `cppc_fi->cpu_data` pointer. Since `cppc_freq_invariance_policy_init()` only set up the per-CPU state but never registered a corresponding teardown, there was no mechanism to cancel pending work or invalidate the pointers on policy exit.

Furthermore, the `topology_set_scale_freq_source()` registration was done globally for `cpu_present_mask` in `cppc_freq_invariance_init()`, but was never updated or deregistered when individual CPUs went offline or their policies were destroyed. This meant the tick callback `cppc_scale_freq_tick()` could continue to be invoked even for CPUs whose CPPC data had been released.

## Consequence

The most severe consequence of this bug was a use-after-free (UAF) condition when accessing `cppc_cpudata` structures after cpufreq policy destruction during CPU hotplug or suspend/resume. This could manifest as:

1. **Kernel crashes (NULL pointer dereference or general protection fault)**: If the freed `cppc_cpudata` memory was reclaimed and zeroed or reallocated, the pointer dereference in `cppc_scale_freq_workfn()` would access garbage data, potentially causing a NULL deref or GPF.
2. **Silent data corruption**: If the freed memory was reallocated for another purpose, the function would read and write incorrect values, corrupting `arch_freq_scale` for the affected CPU. This would cause the scheduler to make incorrect frequency-invariant scheduling decisions.
3. **Kernel hangs**: If `irq_work_sync()` or `kthread_flush_work()` were called at the wrong time or in the wrong order during module unload while hotplug events were in progress, deadlocks could occur.

The bug was reported by Qian Cai, likely observed during stress testing with CPU hotplug cycling or suspend/resume testing on ARM64 platforms with ACPI CPPC firmware. The severity was high enough that the entire feature was reverted rather than attempting a partial fix for the 5.13 release cycle.

## Fix Summary

The fix is a complete revert of commit 4c38f2df71c8 ("cpufreq: CPPC: Add support for frequency invariance"). This removes all of the frequency invariance infrastructure from the CPPC cpufreq driver:

1. **Removed the `CONFIG_ACPI_CPPC_CPUFREQ_FIE` Kconfig option** from `drivers/cpufreq/Kconfig.arm`, eliminating the build-time configuration.
2. **Removed the entire frequency invariance implementation** from `cppc_cpufreq.c`: the `cppc_freq_invariance` per-CPU structure, the `kworker_fie` global kthread worker, the `cppc_scale_freq_workfn()` kthread work function, the `cppc_irq_work()` IRQ work handler, the `cppc_scale_freq_tick()` tick callback, the `cppc_sftd` scale_freq_data registration structure, and the `cppc_freq_invariance_policy_init()`/`cppc_freq_invariance_init()`/`cppc_freq_invariance_exit()` lifecycle functions.
3. **Removed `SCALE_FREQ_SOURCE_CPPC`** from the `scale_freq_source` enum in `include/linux/arch_topology.h`.
4. **Removed `EXPORT_SYMBOL_GPL(sched_setattr_nocheck)`** from `kernel/sched/core.c`, as this export was only needed for the CPPC module to set the `kworker_fie` kthread to `SCHED_DEADLINE` policy.
5. **Re-inlined `cppc_perf_from_fbctrs()` back into `cppc_get_rate_from_fbctrs()`**, as the extracted helper was only needed by the frequency invariance code path.

This revert is correct because it completely eliminates the race conditions by removing all the asynchronous work mechanisms. Without the `irq_work`/`kthread_work` chain, there is nothing to race with policy teardown. The scheduler falls back to non-frequency-invariant scheduling on CPPC platforms, which is a functionality regression but not a correctness issue.

## Triggering Conditions

To trigger this bug, the following conditions must all be met:

- **Hardware/firmware**: An ARM64 system with ACPI CPPC (Collaborative Processor Performance Control) support that exposes delivered and reference performance counters. The HiSilicon workaround path (`hisi_cppc_cpufreq_get_rate`) must NOT be active, as it disables frequency invariance.
- **Kernel configuration**: `CONFIG_ACPI_CPPC_CPUFREQ=y` (or =m), `CONFIG_ACPI_CPPC_CPUFREQ_FIE=y`, and `CONFIG_GENERIC_ARCH_TOPOLOGY=y` must all be enabled.
- **Kernel version**: Linux v5.13-rc1 through v5.13-rc6 (the window between the introduction of the feature and this revert).
- **CPU hotplug or suspend/resume event**: The race requires a cpufreq policy to be torn down while there are in-flight `irq_work` or `kthread_work` items. This happens during CPU hotplug offline operations or system suspend/resume cycles.

The timing window for the race is relatively narrow: between when the scheduler tick queues `irq_work` and when the `kthread_work` completes execution. However, on systems with many CPUs undergoing rapid hotplug cycling (e.g., stress-ng CPU hotplug tests), the probability of hitting the window increases significantly. The `kworker_fie` kthread runs at `SCHED_DEADLINE` priority (with a 10ms period and 1ms runtime budget), so the work items are processed relatively quickly, but not instantaneously — leaving a real race window.

The bug does not require any special userspace workload; it is purely triggered by kernel-internal events (tick + hotplug/suspend interaction) on the right hardware and configuration.

## Reproduce Strategy (kSTEP)

### Why This Bug Cannot Be Reproduced with kSTEP

1. **Requires ACPI CPPC firmware interface**: The entire bug is in the CPPC cpufreq driver (`drivers/cpufreq/cppc_cpufreq.c`), which interfaces with ACPI CPPC firmware to read hardware performance counters. QEMU does not emulate ACPI CPPC firmware, and there is no CPPC cpufreq driver loaded in a QEMU-based kSTEP environment. Without a registered cpufreq driver that uses CPPC counters, the entire frequency invariance code path is unreachable.

2. **Requires CPU hotplug or suspend/resume**: The race condition manifests during cpufreq policy teardown, which occurs during CPU hotplug offline events or system suspend/resume. kSTEP has no API for simulating CPU hotplug (`kstep_cpu_offline()`/`kstep_cpu_online()` do not exist), and QEMU virtual machines in kSTEP do not undergo suspend/resume cycles. The policy exit path cannot be triggered.

3. **Requires registered topology_scale_freq_source callback**: The `cppc_scale_freq_tick()` function must be registered as the active frequency scaling source via `topology_set_scale_freq_source()`. This registration only happens when the CPPC driver's `cppc_freq_invariance_init()` is called successfully, which requires the CPPC cpufreq driver to be loaded and functional.

4. **Bug is in driver code, not scheduler internals**: The only change to `kernel/sched/core.c` is the removal of `EXPORT_SYMBOL_GPL(sched_setattr_nocheck)`. This is a symbol visibility change, not a scheduling logic bug. The actual races are in `drivers/cpufreq/cppc_cpufreq.c`, which is entirely outside the scheduler subsystem and kSTEP's scope.

5. **Requires specific hardware architecture (ARM64)**: CPPC frequency invariance is only functional on ARM64 platforms with ACPI. kSTEP runs on x86_64 (QEMU), where CPPC is not available and the frequency invariance code would never be activated even if the driver were somehow loaded.

### What Would Need to Change in kSTEP

To reproduce this bug, kSTEP would need fundamental additions:
- A way to load and activate a cpufreq driver with custom frequency scaling callbacks in QEMU
- ACPI CPPC firmware emulation in QEMU to provide performance counter reads
- A `kstep_cpu_hotplug(cpu, online/offline)` API to trigger CPU hotplug events
- The ability to run the test on ARM64 QEMU (or emulate CPPC on x86)

These are all fundamental architectural changes, not minor extensions.

### Alternative Reproduction Methods

The bug could potentially be reproduced on real ARM64 hardware (e.g., an Ampere Altra server or Qualcomm Snapdragon platform with ACPI CPPC) running Linux v5.13-rc1 through v5.13-rc6, using a CPU hotplug stress test:
```bash
# Run CPU hotplug stress test
for i in $(seq 1 1000); do
    for cpu in /sys/devices/system/cpu/cpu[1-9]*/online; do
        echo 0 > $cpu; sleep 0.01; echo 1 > $cpu
    done
done
```
Alternatively, rapid suspend/resume cycling could trigger the race.
