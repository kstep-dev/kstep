# Debug: Null-terminated Buffer Missing in tunable_scaling Write

**Commit:** `703066188f63d66cc6b9d678e5b5ef1213c5938e`
**Affected files:** `kernel/sched/debug.c`
**Fixed in:** v5.15-rc4
**Buggy since:** v5.13-rc1 (commit `8a99b6833c88` "sched: Move SCHED_DEBUG sysctl to debugfs")

## Bug Description

When the `SCHED_DEBUG` sysctl interface was migrated from `/proc/sys/kernel/sched_tunable_scaling` to the debugfs path `/sys/kernel/debug/sched/tunable_scaling` in commit `8a99b6833c88`, the new `sched_scaling_write()` handler introduced two bugs: a missing null terminator on the userspace-copied buffer and a missing range validation for the parsed value.

The `sched_scaling_write()` function uses `copy_from_user()` to copy the user-provided string into a stack buffer `buf[16]`, but it never null-terminates the buffer after copying. The `kstrtouint()` function requires a null-terminated C string as input. Without the terminator, `kstrtouint()` reads past the intended data into whatever garbage bytes happen to reside on the stack, and almost certainly encounters non-digit characters, causing it to return `-EINVAL`.

As a result, every attempt to write to `/sys/kernel/debug/sched/tunable_scaling` fails with "Invalid argument", regardless of the value provided. The `tunable_scaling` parameter is effectively read-only, locked to whatever value was set at boot time (default: `SCHED_TUNABLESCALING_LOG`, value 1). This was the only interface for changing this parameter after the migration from procfs.

Additionally, the original code parsed the value directly into the global variable `sysctl_sched_tunable_scaling` without any range check. The valid values are 0 (`SCHED_TUNABLESCALING_NONE`), 1 (`SCHED_TUNABLESCALING_LOG`), and 2 (`SCHED_TUNABLESCALING_LINEAR`), bounded by the sentinel `SCHED_TUNABLESCALING_END` (value 3). If `kstrtouint()` somehow succeeded (e.g., if the stack happened to contain a null byte at the right position), any unsigned integer — including out-of-range values like 5 or 100 — would be written directly to the global variable, potentially corrupting scheduler tuning calculations.

## Root Cause

The root cause is in the `sched_scaling_write()` function in `kernel/sched/debug.c`. The buggy code path is:

```c
static ssize_t sched_scaling_write(struct file *filp, const char __user *ubuf,
                                   size_t cnt, loff_t *ppos)
{
    char buf[16];

    if (cnt > 15)
        cnt = 15;

    if (copy_from_user(&buf, ubuf, cnt))
        return -EFAULT;

    // BUG: buf is NOT null-terminated here.
    // buf contains cnt bytes from userspace, but buf[cnt] is uninitialized stack data.

    if (kstrtouint(buf, 10, &sysctl_sched_tunable_scaling))
        return -EINVAL;
    // kstrtouint() reads past the valid data, finds garbage, returns -EINVAL.

    // BUG: No range check. If kstrtouint succeeded, any value goes straight
    // into sysctl_sched_tunable_scaling.

    if (sched_update_scaling())
        return -EINVAL;
    ...
}
```

When a user runs `echo 0 > /sys/kernel/debug/sched/tunable_scaling`, the shell writes the string `"0\n"` (2 bytes). The function copies these 2 bytes into `buf[0]` and `buf[1]`, but `buf[2]` through `buf[15]` contain whatever was previously on the stack. The `kstrtouint()` function, defined in `lib/kstrtox.c`, calls `_kstrtoull()` which iterates through the string until it encounters a character that is not a valid digit or a null terminator. Since `buf[2]` is uninitialized and almost certainly not `'\0'`, `kstrtouint()` either encounters an invalid character (returning `-EINVAL`) or reads an absurdly large number.

The second sub-bug is that `kstrtouint()` writes directly into `sysctl_sched_tunable_scaling` — the global variable used by the scheduler — without first validating the range. Even though this is masked by the first bug (the parse almost always fails), the code architecture is incorrect: a parse into a temporary variable followed by a range check should precede any write to a global.

## Consequence

The primary observable consequence is that the `/sys/kernel/debug/sched/tunable_scaling` debugfs file becomes effectively read-only after the kernel boots. Any attempt to write a valid value (0, 1, or 2) returns `-EINVAL` to userspace, manifesting as a shell error:

```
$ echo 0 > /sys/kernel/debug/sched/tunable_scaling
-bash: echo: write error: Invalid argument
```

This means system administrators and performance engineers cannot change the `tunable_scaling` policy at runtime. The `tunable_scaling` parameter controls how scheduler tunables (specifically `sched_base_slice` / `sched_min_granularity` / `sched_wakeup_granularity` in the relevant kernel version) are scaled based on the number of CPUs. The three modes are: `NONE` (0) — no scaling; `LOG` (1) — logarithmic scaling (default); `LINEAR` (2) — linear scaling. Being unable to change this parameter means the system is stuck with the boot-time default (`LOG`), which may not be optimal for all workloads.

There is no crash, hang, or scheduler logic corruption under normal circumstances, because `kstrtouint()` fails before writing to the global variable. In the extremely unlikely case that stack contents happen to produce a null byte at the right position and `kstrtouint()` succeeds, an out-of-range value could be written to `sysctl_sched_tunable_scaling`. The `sched_update_scaling()` function uses this value via `get_update_sysctl_factor()`, which indexes into a scaling table. An out-of-range index would produce an undefined scaling factor, potentially corrupting scheduler latency parameters. However, this scenario is practically unreachable.

## Fix Summary

The fix applies three changes to `sched_scaling_write()`:

1. **Null-terminate the buffer**: After `copy_from_user()`, the fix adds `buf[cnt] = '\0';` to properly null-terminate the string. This allows `kstrtouint()` to parse only the intended bytes and return success for valid numeric inputs.

2. **Parse into a temporary variable**: Instead of parsing directly into the global `sysctl_sched_tunable_scaling`, the fix introduces a local variable `unsigned int scaling` and parses into that: `kstrtouint(buf, 10, &scaling)`. This ensures the global variable is never modified with an unvalidated value.

3. **Add range validation**: After successful parsing, the fix checks `if (scaling >= SCHED_TUNABLESCALING_END) return -EINVAL;` to reject out-of-range values. Only after this check passes does it assign `sysctl_sched_tunable_scaling = scaling;` and proceed to call `sched_update_scaling()`.

The fix is complete and correct. It follows the same defensive pattern used in other debugfs write handlers in the scheduler (e.g., `sched_dynamic_write()`), ensuring proper string handling, temporary-variable parsing, and range validation before modifying any global state.

## Triggering Conditions

The bug triggers under the following conditions:

- **Kernel version**: Any kernel between v5.13-rc1 (commit `8a99b6833c88`) and v5.15-rc3 (before the fix was applied) with `CONFIG_SCHED_DEBUG` enabled and `CONFIG_SMP` enabled (the `sched_scaling_write` function is guarded by `#ifdef CONFIG_SMP`).
- **Action**: Any write to the debugfs file `/sys/kernel/debug/sched/tunable_scaling`, regardless of the value written. Both valid values (0, 1, 2) and invalid values trigger the same `-EINVAL` failure.
- **Userspace access**: The bug requires userspace access to debugfs, which is mounted at `/sys/kernel/debug/` (typically requires root or `CAP_SYS_ADMIN`).
- **Reproduction is 100% deterministic**: Every write attempt fails because uninitialized stack data virtually never contains a null byte at the exact position needed for `kstrtouint()` to succeed.

No special CPU topology, task configuration, workload characteristics, or timing requirements are needed. The bug is a simple programming error in the debugfs write handler that causes unconditional failure.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. The reasons are:

1. **Requires userspace file I/O to debugfs**: The bug is in the `sched_scaling_write()` function, which is a debugfs file operation handler invoked when userspace writes to `/sys/kernel/debug/sched/tunable_scaling`. The bug is specifically in how userspace-provided data is processed after `copy_from_user()`. kSTEP operates as a kernel module and does not have a built-in mechanism for performing userspace-style file writes to debugfs.

2. **No debugfs write API in kSTEP**: kSTEP provides `kstep_sysctl_write(name, fmt, ...)` for writing to procfs sysctl entries (`/proc/sys/...`), but the `tunable_scaling` parameter was moved from procfs to debugfs in the commit that introduced this bug. There is no `kstep_debugfs_write()` API. While a kernel module could theoretically use `filp_open()` + `kernel_write()` to write to the debugfs file, this is a VFS-level file operation that kSTEP does not abstract.

3. **No scheduler logic error to observe**: The fundamental problem is that this is not a scheduler logic bug. The scheduler itself operates correctly at all times. The CFS/EEVDF scheduling algorithm, load balancing, task placement, and all other scheduler behaviors are completely unaffected. The only symptom is that the debugfs write handler returns `-EINVAL` when it should succeed. There is no task starvation, priority inversion, incorrect vruntime computation, or any other scheduling anomaly that a kSTEP driver could detect through the scheduler observation APIs (`kstep_eligible()`, `kstep_output_curr_task()`, `kstep_output_nr_running()`, etc.).

4. **Bypassing the buggy code defeats the purpose**: One might consider using `KSYM_IMPORT(sysctl_sched_tunable_scaling)` to directly write to the global variable from a kernel module, but this would bypass the buggy `sched_scaling_write()` handler entirely. The bug is in the *input parsing path*, not in the scheduler's use of the variable. Directly modifying the variable would not exercise the buggy code.

5. **What would need to change in kSTEP**: To support this, kSTEP would need a `kstep_debugfs_write(path, fmt, ...)` helper that performs `filp_open()` + `kernel_write()` + `filp_close()` on arbitrary debugfs files. This is a minor addition, but the resulting test would merely verify that a file write returns 0 (success) on the fixed kernel and `-EINVAL` on the buggy kernel. This is a functional test of a file I/O path, not a scheduler behavior test, which is outside kSTEP's design intent.

6. **Alternative reproduction methods**: The bug is trivially reproducible outside kSTEP by simply running `echo 0 > /sys/kernel/debug/sched/tunable_scaling` on an affected kernel. If the command fails with "Invalid argument", the bug is present. If it succeeds and `cat /sys/kernel/debug/sched/tunable_scaling` shows 0, the bug is fixed. This requires only shell access to a system running an affected kernel version with `CONFIG_SCHED_DEBUG` and debugfs mounted.
