# Cgroup: Autogroup Sysctl Registration Missing After Init Refactor

**Commit:** `82f586f923e3ac6062bc7867717a7f8afc09e0ff`
**Affected files:** kernel/sched/autogroup.c
**Fixed in:** v5.19-rc1
**Buggy since:** v5.18-rc1 (introduced by commit `c8eaf6ac76f4` "sched: move autogroup sysctls into its own file")

## Bug Description

The scheduler autogroup feature allows the kernel to automatically group tasks by TTY session into separate CFS task groups, improving interactive desktop responsiveness. The feature is controlled at runtime via the sysctl `/proc/sys/kernel/sched_autogroup_enabled`, which allows administrators to enable or disable autogroup without rebooting.

In Linux v5.18-rc1, commit `c8eaf6ac76f4` refactored the autogroup sysctl registration by moving it from the central `kernel/sysctl.c` table into `kernel/sched/autogroup.c` itself, using the newer `register_sysctl_init()` API. However, this refactor placed the call to `sched_autogroup_sysctl_init()` in the wrong location: it was added inside `setup_autogroup()`, the `__setup("noautogroup", ...)` early boot parameter handler, instead of in `autogroup_init()`, the standard initialization function that is always called during boot.

As a result, when the kernel boots normally (without the `noautogroup` parameter), the function `sched_autogroup_sysctl_init()` is never called, and the sysctl entry `/proc/sys/kernel/sched_autogroup_enabled` is never registered. This means the proc file simply does not exist. Additionally, when booting with `noautogroup`, the sysctl registration is attempted during early `__setup` processing, which occurs before the sysctl subsystem is fully initialized, resulting in a "failed when register_sysctl sched_autogroup_sysctls to kernel" error message.

The autogroup scheduling mechanism itself still functions correctly (the `sysctl_sched_autogroup_enabled` variable is initialized to 1 at compile time), but runtime control via the sysctl interface is completely broken.

## Root Cause

The root cause lies in the incorrect placement of the `sched_autogroup_sysctl_init()` call during the refactoring in commit `c8eaf6ac76f4`. The original code in `kernel/sysctl.c` used a static `ctl_table` array in the global `kern_table[]`, which was registered automatically as part of the kernel's sysctl infrastructure during boot. When the sysctl definition was moved to `kernel/sched/autogroup.c`, the new code defined a local `sched_autogroup_sysctls[]` table and a helper function `sched_autogroup_sysctl_init()` that calls `register_sysctl_init("kernel", sched_autogroup_sysctls)` to register it.

The critical error was that `sched_autogroup_sysctl_init()` was placed inside the `setup_autogroup()` function:

```c
static int __init setup_autogroup(char *str)
{
    sysctl_sched_autogroup_enabled = 0;
    sched_autogroup_sysctl_init();   /* <-- BUG: only called with noautogroup */
    return 1;
}
__setup("noautogroup", setup_autogroup);
```

The `__setup` mechanism means `setup_autogroup()` is only called if the `noautogroup` parameter is present on the kernel command line. For the vast majority of boots (without `noautogroup`), this function is never invoked, and therefore `sched_autogroup_sysctl_init()` never runs. The sysctl is simply never registered.

Even in the `noautogroup` case, the placement is problematic. The `__setup` handlers run very early during boot (during `parse_early_param()` / `parse_args()` in `start_kernel()`), before many subsystems are initialized. While `register_sysctl_init()` is designed to defer registration, the early call still triggers an error because the kernel's sysctl tree may not be fully set up at that point. This produces the error message: "failed when register_sysctl sched_autogroup_sysctls to kernel".

The correct location for the `sched_autogroup_sysctl_init()` call is inside `autogroup_init()`, which is called unconditionally from `sched_init()` during normal kernel initialization:

```c
void __init autogroup_init(struct task_struct *init_task)
{
    autogroup_default.tg = &root_task_group;
    kref_init(&autogroup_default.kref);
    init_rwsem(&autogroup_default.lock);
    init_task->signal->autogroup = &autogroup_default;
    /* sched_autogroup_sysctl_init() should be called HERE */
}
```

This function always runs during boot, at the appropriate point in the initialization sequence when the sysctl subsystem can accept registrations.

## Consequence

The most immediate and visible consequence is that the sysctl file `/proc/sys/kernel/sched_autogroup_enabled` is completely absent from the system. Any userspace tool or script that reads or writes this sysctl (e.g., `sysctl kernel.sched_autogroup_enabled`, or `cat /proc/sys/kernel/sched_autogroup_enabled`) fails with "No such file or directory". System administrators lose the ability to enable or disable autogroup at runtime, and monitoring tools that inspect this sysctl will report errors or incorrect system state.

When the `noautogroup` boot parameter is used, the kernel prints an error message at boot: "failed when register_sysctl sched_autogroup_sysctls to kernel". This is alarming to administrators who see unexpected error messages in kernel logs. Moreover, even with `noautogroup`, the sysctl file still does not appear (since the early registration fails), so it is impossible to verify the autogroup state or re-enable it at runtime.

Importantly, the underlying autogroup scheduling behavior is not affected: the `sysctl_sched_autogroup_enabled` variable is initialized to 1 at compile time, so autogroup is functionally enabled by default. Tasks continue to be grouped by TTY session for CFS scheduling purposes. However, the loss of runtime control means administrators cannot disable autogroup when it causes undesired behavior (e.g., on servers where TTY-based grouping is irrelevant or counterproductive), nor can they re-enable it if they booted with `noautogroup` and later want to turn it on. This is a user-facing regression that breaks the sysctl configuration interface.

## Fix Summary

The fix commit `82f586f923e3` makes a minimal two-line change to `kernel/sched/autogroup.c`. It moves the `sched_autogroup_sysctl_init()` call from `setup_autogroup()` (the `noautogroup` command-line handler) to `autogroup_init()` (the standard initialization function).

Specifically, the fix adds `sched_autogroup_sysctl_init()` at the end of `autogroup_init()`, after the autogroup default structure has been fully initialized:

```c
void __init autogroup_init(struct task_struct *init_task)
{
    autogroup_default.tg = &root_task_group;
    kref_init(&autogroup_default.kref);
    init_rwsem(&autogroup_default.lock);
    init_task->signal->autogroup = &autogroup_default;
    sched_autogroup_sysctl_init();  /* <-- FIX: always register the sysctl */
}
```

And simultaneously removes the call from `setup_autogroup()`:

```c
static int __init setup_autogroup(char *str)
{
    sysctl_sched_autogroup_enabled = 0;
    /* sched_autogroup_sysctl_init() removed from here */
    return 1;
}
```

This fix is correct because `autogroup_init()` is always called during boot (from `sched_init()` in `kernel/sched/core.c`), regardless of whether `noautogroup` is passed. It runs at the proper time in the kernel initialization sequence when the sysctl subsystem is ready to accept registrations. The `setup_autogroup()` handler now only needs to set `sysctl_sched_autogroup_enabled = 0`, which is its intended purpose — the sysctl registration is handled separately and unconditionally.

## Triggering Conditions

The bug is triggered unconditionally on any Linux v5.18 kernel compiled with `CONFIG_SCHED_AUTOGROUP=y` and `CONFIG_SYSCTL=y` (both are standard defaults on virtually all distributions). No special hardware, CPU topology, or workload is required.

The two manifestations of the bug have distinct triggering conditions:
1. **Missing sysctl (default boot):** Simply boot a v5.18 kernel without the `noautogroup` parameter. The sysctl `/proc/sys/kernel/sched_autogroup_enabled` will not exist. This affects every default v5.18 boot.
2. **Boot error message (noautogroup boot):** Boot a v5.18 kernel with the `noautogroup` command line parameter. The kernel will print "failed when register_sysctl sched_autogroup_sysctls to kernel" during early boot. The sysctl will also not exist.

The bug is 100% deterministic and requires no race conditions, specific timing, or concurrency. It is a straightforward init-ordering mistake that manifests on every single boot of an affected kernel.

The minimum configuration requirements are:
- `CONFIG_SCHED_AUTOGROUP=y` (enabled by default on most distributions)
- `CONFIG_SYSCTL=y` (enabled by default on virtually all configurations)
- Any number of CPUs (1 or more)
- Any amount of RAM
- No special topology requirements

## Reproduce Strategy (kSTEP)

This bug is a boot-time initialization ordering issue rather than a runtime scheduler logic bug. The sysctl `/proc/sys/kernel/sched_autogroup_enabled` is either registered or not by the time the kSTEP kernel module loads, so the bug is already present (or absent) from the moment the system finishes booting. The reproduction strategy therefore focuses on detecting the presence or absence of the sysctl file.

**Step 1: Kernel checkout.** Checkout the buggy kernel at `82f586f923e3~1` (the commit just before the fix). This gives us a v5.18 kernel where the bug is present. Ensure the kernel config includes `CONFIG_SCHED_AUTOGROUP=y` and `CONFIG_SYSCTL=y`.

**Step 2: Driver implementation.** Create a driver `cgroup_autogroup_sysctl_missing.c` in `kmod/drivers_generated/`. The driver does not need to create any tasks, cgroups, or special topology. It only needs to check whether the sysctl file exists. Guard the driver with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 19, 0)` to target the affected kernel range (though it can be broader for testing the fix).

**Step 3: Detection approach.** Since `kstep_sysctl_write()` calls `panic()` on failure (when the file doesn't exist), we cannot use it directly to probe the sysctl. Instead, the driver should use `filp_open()` directly to attempt to open `/proc/sys/kernel/sched_autogroup_enabled` with `O_RDONLY` and check if the return value is an error:

```c
struct file *f = filp_open("/proc/sys/kernel/sched_autogroup_enabled", O_RDONLY, 0);
if (IS_ERR(f)) {
    kstep_fail("sysctl sched_autogroup_enabled is missing (not registered)");
} else {
    filp_close(f, NULL);
    kstep_pass("sysctl sched_autogroup_enabled exists and is accessible");
}
```

**Step 4: Optional extended check.** To additionally verify that the sysctl is writable and functional, the driver can attempt a `kernel_read()` from the opened file to read the current value of `sysctl_sched_autogroup_enabled`, and then use `kernel_write()` to toggle it (e.g., write "0\n" then "1\n") and verify the underlying variable `sysctl_sched_autogroup_enabled` (imported via `KSYM_IMPORT`) changes accordingly. This validates not just that the file exists, but that it is functional.

**Step 5: Expected behavior on buggy kernel.** On the buggy kernel (`82f586f923e3~1`), `filp_open("/proc/sys/kernel/sched_autogroup_enabled", ...)` will return `-ENOENT` because the sysctl was never registered. The driver should report `kstep_fail()`.

**Step 6: Expected behavior on fixed kernel.** On the fixed kernel (`82f586f923e3`), `filp_open("/proc/sys/kernel/sched_autogroup_enabled", ...)` will succeed, returning a valid `struct file *`. The driver should report `kstep_pass()`.

**Step 7: Build and run.** Build and run with:
```
./checkout_linux.py 82f586f923e3~1 autogroup_buggy
make linux LINUX_NAME=autogroup_buggy
./run.py cgroup_autogroup_sysctl_missing --linux_name autogroup_buggy
```
Then verify the fix:
```
./checkout_linux.py 82f586f923e3 autogroup_fixed
make linux LINUX_NAME=autogroup_fixed
./run.py cgroup_autogroup_sysctl_missing --linux_name autogroup_fixed
```

**Step 8: Additional scheduling impact verification.** While the primary detection is the missing sysctl file, an extended version of the driver could demonstrate the scheduling impact by:
1. Creating two CFS tasks in different TTY-based autogroups
2. Attempting to disable autogroup via the sysctl (which fails on the buggy kernel)
3. Observing that the tasks remain in separate autogroups even after attempting to disable the feature
4. On the fixed kernel, confirming that writing 0 to the sysctl successfully disables autogroup and the tasks are no longer grouped

However, this extended verification is complex to set up in kSTEP since autogroup assignment is based on the controlling TTY of the process, which kSTEP's kernel-controlled tasks may not have. The simple file existence check is the most reliable and straightforward detection method.

**QEMU configuration:** No special QEMU configuration is needed. The default configuration with 2+ CPUs is sufficient. This bug does not depend on CPU count, topology, or memory size.

**Determinism:** This reproduction is fully deterministic. The bug is a static initialization error — the sysctl is either registered or not, with no timing or race conditions involved. Every run on the buggy kernel will fail, and every run on the fixed kernel will pass.
