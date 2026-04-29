# Core: Boot crash when boot CPU is in nohz_full mask

**Commit:** `5097cbcb38e6e0d2627c9dde1985e91d2c9f880e`
**Affected files:** kernel/sched/isolation.c, Documentation/timers/no_hz.rst
**Fixed in:** v6.9-rc6
**Buggy since:** v6.9-rc1 (introduced by commit `aae17ebb53cd` "workqueue: Avoid using isolated cpus' timers on queue_delayed_work")

## Bug Description

When the kernel is booted with a `nohz_full=` mask that includes the boot CPU (e.g., `nohz_full=0-6` on an 8-CPU system), the kernel crashes during early boot. The crash occurs because `housekeeping_any_cpu(HK_TYPE_TIMER)` returns an invalid CPU number (`>= nr_cpu_ids`, effectively `NR_CPUS`) to callers that assume a valid CPU is always returned.

The root problem is a sequence of two commits interacting badly. First, commit `08ae95f4fd3b` ("nohz_full: Allow the boot CPU to be nohz_full") made it legal for the boot CPU to be included in the `nohz_full=` mask — previously, the boot CPU was explicitly excluded from adaptive-ticks mode. Second, commit `aae17ebb53cd` ("workqueue: Avoid using isolated cpus' timers on queue_delayed_work") changed `__queue_delayed_work()` to call `housekeeping_any_cpu(HK_TYPE_TIMER)` to find a non-isolated CPU for timer placement. This function was not designed to handle the case where no housekeeping CPU is online yet.

During very early boot, only the boot CPU (CPU 0) is online. If CPU 0 is marked as `nohz_full` (i.e., it is NOT a housekeeping CPU), then the housekeeping cpumask for `HK_TYPE_TIMER` contains only the non-nohz_full CPUs (e.g., CPU 7 in the example above). However, since `smp_init()` has not yet run, CPU 7 is not yet online. The intersection of `housekeeping.cpumasks[HK_TYPE_TIMER]` and `cpu_online_mask` is therefore empty, causing `cpumask_any_and()` to return `>= nr_cpu_ids`.

This invalid CPU number is then passed to `add_timer_on()`, which dereferences an out-of-bounds per-CPU timer base pointer, causing a crash. The bug is deterministic: any system booted with the boot CPU included in `nohz_full=` will crash if `__queue_delayed_work()` is called before the first housekeeping CPU comes online.

## Root Cause

The function `housekeeping_any_cpu()` in `kernel/sched/isolation.c` is responsible for returning a valid housekeeping CPU for a given housekeeping type. Its implementation has two lookup strategies:

```c
int housekeeping_any_cpu(enum hk_type type)
{
    int cpu;
    if (static_branch_unlikely(&housekeeping_overridden)) {
        if (housekeeping.flags & BIT(type)) {
            cpu = sched_numa_find_closest(housekeeping.cpumasks[type], smp_processor_id());
            if (cpu < nr_cpu_ids)
                return cpu;
            return cpumask_any_and(housekeeping.cpumasks[type], cpu_online_mask);
        }
    }
    return smp_processor_id();
}
```

First, it tries `sched_numa_find_closest()` to find a NUMA-close housekeeping CPU. If that fails (returns `>= nr_cpu_ids`), it falls back to `cpumask_any_and(housekeeping.cpumasks[type], cpu_online_mask)`, which finds any online housekeeping CPU. The critical flaw is that this second fallback does **not** check whether the returned value is valid (i.e., `< nr_cpu_ids`). If the intersection is empty — which happens during early boot when no housekeeping CPU is online yet — `cpumask_any_and()` returns `>= nr_cpu_ids`, and this invalid value is returned directly to the caller.

Before commit `aae17ebb53cd`, the workqueue code did not call `housekeeping_any_cpu()` during delayed work queueing, so this code path was not exercised during early boot with `nohz_full=` including the boot CPU. After that commit, `__queue_delayed_work()` in `kernel/workqueue.c` was changed to:

```c
if (housekeeping_enabled(HK_TYPE_TIMER)) {
    cpu = smp_processor_id();
    if (!housekeeping_test_cpu(cpu, HK_TYPE_TIMER))
        cpu = housekeeping_any_cpu(HK_TYPE_TIMER);
    add_timer_on(timer, cpu);
}
```

When the current CPU (the boot CPU) is not a housekeeping CPU (because it's in `nohz_full=`), the code calls `housekeeping_any_cpu(HK_TYPE_TIMER)`. During early boot, this returns `NR_CPUS` because no housekeeping CPU is online. The invalid value is then passed to `add_timer_on()`, which uses it to index per-CPU data, resulting in an out-of-bounds memory access and crash.

The fundamental issue is that `housekeeping_any_cpu()` has an implicit contract that there is always at least one online housekeeping CPU, which is violated during the window between early boot (when only the boot CPU is online) and `smp_init()` (when secondary CPUs are brought up). This window exists specifically because commit `08ae95f4fd3b` allowed the boot CPU to be `nohz_full`, meaning the boot CPU is not necessarily a housekeeping CPU.

## Consequence

The consequence is a **hard kernel crash during boot** — the system fails to boot entirely. The crash occurs when `add_timer_on()` is called with an invalid CPU number (`NR_CPUS` or higher). This causes an out-of-bounds access into per-CPU timer base structures, typically manifesting as a NULL pointer dereference or other memory corruption.

The crash is deterministic and 100% reproducible: any system booted with `nohz_full=` including the boot CPU will crash during early boot, before the first secondary (housekeeping) CPU is brought online. The crash affects the `__queue_delayed_work()` path, which is used extensively by the workqueue subsystem during kernel initialization.

This is a regression introduced in v6.9-rc1 by the workqueue commit `aae17ebb53cd`. It affects any system that combines `CONFIG_NO_HZ_FULL=y` with a `nohz_full=` boot parameter that includes the boot CPU. Real-time and latency-sensitive systems commonly use `nohz_full=` to isolate CPUs for critical workloads, and the documentation (updated by commit `08ae95f4fd3b`) allows the boot CPU to be included. The crash makes such configurations completely unusable.

## Fix Summary

The fix modifies `housekeeping_any_cpu()` to validate the return value of `cpumask_any_and()` before returning it. If no online housekeeping CPU is found (i.e., the result is `>= nr_cpu_ids`), the function falls through to return `smp_processor_id()` instead. This is safe because during early boot, `smp_processor_id()` returns the boot CPU, which is the only CPU running and can handle timers regardless of its nohz_full status.

The fix also adds a `WARN_ON_ONCE()` diagnostic that fires if this fallback is reached outside of early boot conditions. Specifically, it warns if `system_state == SYSTEM_RUNNING` (meaning boot is complete, so all housekeeping CPUs should be online) or if the housekeeping type is not `HK_TYPE_TIMER` (since the known trigger is specifically the timer housekeeping path). This ensures that if the fallback is exercised in unexpected situations, developers are alerted.

Additionally, the fix updates `Documentation/timers/no_hz.rst` to remove the outdated statement that the boot CPU is prohibited from entering adaptive-ticks mode. This documentation was already stale since commit `08ae95f4fd3b` allowed the boot CPU to be `nohz_full`, and the old documentation could mislead users into thinking the configuration is invalid when it is actually supported.

## Triggering Conditions

The bug requires the following precise conditions:

1. **Kernel configuration:** `CONFIG_NO_HZ_FULL=y` must be enabled in the kernel configuration.
2. **Boot parameter:** The `nohz_full=` boot parameter must include the boot CPU (typically CPU 0). For example, `nohz_full=0-6` on an 8-CPU system, where CPUs 0-6 are nohz_full and only CPU 7 is a housekeeping CPU.
3. **Timing:** The crash occurs during early kernel initialization, specifically before `smp_init()` brings secondary CPUs online. The trigger is any call to `__queue_delayed_work()` that invokes `housekeeping_any_cpu(HK_TYPE_TIMER)` during this window.
4. **Multi-CPU system:** The system must have at least 2 CPUs (otherwise `nohz_full` has no effect).

The bug is deterministic — there is no race condition or timing sensitivity beyond the boot sequence ordering. If the boot CPU is in `nohz_full=` and any delayed work is queued before secondary CPUs come up (which happens routinely during kernel init), the crash will occur every time.

The bug does NOT require any specific hardware, architecture, or NUMA topology. It can be triggered on any architecture that supports `CONFIG_NO_HZ_FULL`, including x86_64, ARM64, etc.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following fundamental reasons:

### 1. Why kSTEP Cannot Reproduce This Bug

The bug is a **boot-time crash** that occurs during the very early stages of kernel initialization, before `smp_init()` brings secondary CPUs online. kSTEP operates as a kernel module that is loaded **after** the kernel has fully booted and all CPUs are online. By the time a kSTEP driver executes:

- All CPUs specified in the QEMU configuration are already online and in `SYSTEM_RUNNING` state.
- The housekeeping cpumask has already been intersected with `cpu_online_mask`, and all housekeeping CPUs are reachable.
- `housekeeping_any_cpu()` will always find at least one valid online housekeeping CPU, so the bug condition (empty intersection) cannot occur.

Additionally, kSTEP cannot configure kernel boot parameters. The `nohz_full=` parameter is parsed during early kernel init by `__setup("nohz_full=", housekeeping_nohz_full_setup)` and cannot be changed at runtime. The `kstep_sysctl_write()` API can modify runtime sysctls, but the nohz_full mask is fixed at boot time.

### 2. What Would Be Needed to Reproduce This

To reproduce this bug, the test framework would need:

- **Boot parameter injection:** The ability to pass arbitrary kernel command-line parameters (e.g., `nohz_full=0-6`) to the QEMU guest kernel. This is a QEMU/boot configuration change, not a kSTEP API change.
- **Early boot instrumentation:** The ability to run test code during the kernel initialization sequence, before `smp_init()`. This would require either a built-in (non-loadable) kernel module or modifications to the kernel init code itself — fundamentally different from kSTEP's loadable-module architecture.
- **Boot crash detection:** The ability to detect that the kernel crashed during boot (never reached the point of loading modules). This requires monitoring the QEMU guest from the host side.

These are not minor extensions to kSTEP — they require a fundamentally different testing architecture that can operate during kernel initialization rather than after boot completion.

### 3. Alternative Reproduction Methods

The bug can be reproduced outside kSTEP using standard QEMU boot testing:

1. Build a kernel with `CONFIG_NO_HZ_FULL=y`.
2. Boot QEMU with multiple CPUs (e.g., `-smp 8`) and the kernel parameter `nohz_full=0-6`.
3. Observe that the kernel crashes during boot, before reaching userspace.
4. Apply the fix commit and verify the kernel boots successfully.

A simple QEMU boot test script would suffice:
```bash
qemu-system-x86_64 -kernel bzImage -smp 8 -m 512M \
    -append "nohz_full=0-6 console=ttyS0" -nographic -no-reboot
```

On the buggy kernel, QEMU will show a crash/panic during early boot. On the fixed kernel, the boot will complete normally. The fix adds a `WARN_ON_ONCE` that would appear in dmesg if the fallback path is ever taken during boot, providing additional verification.
