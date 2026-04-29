# Core: Dynamic preempt __setup() callback returns swapped success/failure values

**Commit:** `9ed20bafc85806ca6c97c9128cec46c3ef80ae86`
**Affected files:** kernel/sched/core.c
**Fixed in:** v5.16-rc4
**Buggy since:** v5.12-rc1 (introduced by commit `826bfeb37bb4` "preempt/dynamic: Support dynamic preempt with preempt= boot option")

## Bug Description

The `setup_preempt_mode()` function, registered via the `__setup("preempt=", ...)` macro as a kernel boot parameter handler, had its return values inverted. The `__setup()` infrastructure in the Linux kernel expects callbacks to return `1` to indicate the parameter was successfully consumed, and `0` to indicate failure (the parameter was not recognized or could not be handled). The buggy code returned `0` on success and `1` on failure, which is the opposite of the expected convention.

This bug was introduced in commit `826bfeb37bb4` which added the `preempt=` boot option to allow users to dynamically select a preemption model (`none`, `voluntary`, or `full`) at boot time via the kernel command line. The commit's author apparently confused the return value convention — possibly influenced by the common C convention where 0 means success — and wrote the returns backwards.

The bug affects every kernel booted with the `preempt=` command-line option when `CONFIG_PREEMPT_DYNAMIC` is enabled. While the actual preemption mode is still correctly applied (the call to `sched_dynamic_update(mode)` executes before the return), the incorrect return value causes the `__setup` infrastructure to misinterpret the result, leading to spurious warnings or silenced errors.

## Root Cause

The `setup_preempt_mode()` function in `kernel/sched/core.c` is a `__setup()` callback responsible for parsing the `preempt=` kernel command-line argument. The function calls `sched_dynamic_mode(str)` to convert the string argument (e.g., "none", "voluntary", "full") into an integer mode constant. If the mode string is invalid, `sched_dynamic_mode()` returns a negative value.

In the buggy code, the function was:

```c
static int __init setup_preempt_mode(char *str)
{
    int mode = sched_dynamic_mode(str);
    if (mode < 0) {
        pr_warn("Dynamic Preempt: unsupported mode: %s\n", str);
        return 1;  // BUG: tells __setup infrastructure "handled successfully"
    }

    sched_dynamic_update(mode);
    return 0;  // BUG: tells __setup infrastructure "not handled"
}
```

The `__setup()` infrastructure in `init/main.c` processes the return value as follows: if the callback returns `1`, the parameter is considered consumed and no further processing occurs. If the callback returns `0`, the parameter is treated as unrecognized and is passed through to init as an environment variable. Additionally, when `CONFIG_BOOT_CONFIG` is not set or the parameter isn't a boot config parameter, returning `0` results in the kernel printing a warning: `"Unknown kernel command line parameters"`.

The root cause is a simple logic inversion of the return value convention. The developer likely followed the common C/POSIX convention where `0` means success, but the `__setup()` callback convention is the opposite: `1` means "I handled this parameter" and `0` means "I did not handle this parameter."

## Consequence

When a user boots the kernel with a **valid** `preempt=` option (e.g., `preempt=none`, `preempt=voluntary`, or `preempt=full`), two things happen:

1. The preemption mode **is** correctly set — `sched_dynamic_update(mode)` executes before the return, so the dynamic preemption static calls are patched to the requested mode. Functionally, the scheduler behaves correctly.
2. However, the function returns `0`, which causes the `__setup` infrastructure to treat the parameter as unhandled. This results in the kernel printing `"Unknown kernel command line parameters 'preempt=<mode>'"` to the console, and attempting to pass `preempt=<mode>` as an environment variable to the init process. This is confusing to users who see the warning despite using a valid parameter.

When a user boots with an **invalid** `preempt=` option (e.g., `preempt=bogus`), the function prints the `pr_warn` message about the unsupported mode but then returns `1`, telling the infrastructure the parameter was handled successfully. This means the parameter is silently swallowed — the init process is not informed of the unrecognized parameter, and the `"Unknown kernel command line parameters"` warning is suppressed, which masks the configuration error.

The bug does not cause crashes, data corruption, or incorrect scheduling behavior. It is a cosmetic/diagnostic issue: valid parameters produce spurious warnings, and invalid parameters are silently accepted by the `__setup` infrastructure.

## Fix Summary

The fix in commit `9ed20bafc85806ca6c97c9128cec46c3ef80ae86` simply swaps the two return values to match the `__setup()` callback convention:

```c
static int __init setup_preempt_mode(char *str)
{
    int mode = sched_dynamic_mode(str);
    if (mode < 0) {
        pr_warn("Dynamic Preempt: unsupported mode: %s\n", str);
        return 0;  // FIX: failure — tell __setup infrastructure "not handled"
    }

    sched_dynamic_update(mode);
    return 1;  // FIX: success — tell __setup infrastructure "handled"
}
```

After the fix, a valid `preempt=` option returns `1` (handled), so the `__setup` infrastructure knows the parameter was consumed and does not produce any warning. An invalid `preempt=` option returns `0` (not handled), so the infrastructure correctly flags it as an unknown parameter, alerting the user to the misconfiguration.

The fix is minimal (2 lines changed), correct, and complete. No functional scheduler behavior is altered — only the reporting to the boot parameter infrastructure is corrected.

## Triggering Conditions

The bug requires the following conditions to manifest:

- **Kernel configuration:** `CONFIG_PREEMPT_DYNAMIC=y` must be enabled, which allows runtime selection of the preemption model. This config option was introduced in v5.12 and is commonly enabled in distribution kernels.
- **Boot parameter:** The kernel must be booted with the `preempt=` command-line parameter (e.g., `preempt=none`, `preempt=voluntary`, or `preempt=full`). Without this parameter, `setup_preempt_mode()` is never called and the bug is not triggered.
- **Observable symptom for valid input:** After booting with a valid `preempt=` mode, the kernel log (`dmesg`) will contain a `"Unknown kernel command line parameters"` warning. The init process may also receive `preempt=<mode>` as an environment variable.
- **Observable symptom for invalid input:** After booting with an invalid `preempt=` mode (e.g., `preempt=bogus`), the `pr_warn` message from `setup_preempt_mode()` will be printed, but no `"Unknown kernel command line parameters"` warning will appear from the `__setup` infrastructure, masking the error.

The bug is 100% deterministic — it triggers on every boot where the `preempt=` parameter is used. There are no race conditions, timing dependencies, or CPU count requirements. It is purely a boot-time initialization issue triggered by a specific kernel command-line argument.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. Here is the detailed rationale:

### 1. Why kSTEP Cannot Reproduce This Bug

The bug exists solely in the `__setup()` callback path, which is part of the kernel's early boot initialization. The `setup_preempt_mode()` function is declared with `__init`, meaning it is called exactly once during kernel boot when the kernel parses command-line parameters, and its code is freed from memory afterward. kSTEP operates by loading a kernel module after the kernel has fully booted, at which point:

- The `setup_preempt_mode()` function has already been called (or not called, if no `preempt=` parameter was given) and its memory has been freed.
- The `__setup` infrastructure has already processed all boot parameters and cannot be re-invoked.
- A kernel module cannot influence or observe the return value of `__init` functions.

The bug has no effect on any runtime scheduler state that a kernel module could inspect. The actual preempt mode is correctly set regardless of the return value — the bug only affects the communication between the `__setup()` callback and the boot parameter parsing infrastructure. There is no scheduler data structure, runqueue field, or task attribute that reflects this bug after boot.

### 2. What Would Need to Be Added to kSTEP

Reproducing this bug would require fundamentally different capabilities that go beyond kSTEP's kernel-module-based architecture:

- **Boot parameter injection:** kSTEP would need the ability to specify kernel command-line parameters passed to QEMU (e.g., appending `preempt=voluntary` to the `-append` argument). While QEMU supports this, kSTEP's current `run.py` infrastructure would need to be extended to allow per-driver custom kernel boot arguments.
- **Boot-time log capture:** kSTEP would need to capture the kernel boot log (dmesg) from very early in the boot process and make it available for analysis. The pass/fail criteria would be checking for the presence or absence of `"Unknown kernel command line parameters"` in the boot log.
- **`__init` function instrumentation:** Alternatively, kSTEP could instrument `__init` functions before they are freed, but this would require patching the kernel build process or using kprobes early enough in boot, which is impractical.

These changes would require modifying kSTEP's boot infrastructure (QEMU arguments, boot log parsing) rather than just adding new module-level APIs, making this a fundamental architectural change.

### 3. Classification

This bug is classified as **unplanned** because it is a boot-time initialization issue that cannot be triggered or observed from a post-boot kernel module. The bug does not corrupt any scheduler data structure and has no runtime scheduling impact — it only affects the kernel's handling of the `preempt=` command-line parameter during early boot.

### 4. Alternative Reproduction Methods

The bug can be trivially reproduced outside kSTEP:

1. Build a kernel with `CONFIG_PREEMPT_DYNAMIC=y` using the buggy version (v5.12-rc1 through v5.16-rc3).
2. Boot the kernel in QEMU with: `qemu-system-x86_64 -kernel bzImage -append "preempt=voluntary console=ttyS0" -nographic`
3. Check `dmesg` output for the spurious `"Unknown kernel command line parameters 'preempt=voluntary'"` warning.
4. Verify the preempt mode was actually set correctly: `cat /sys/kernel/debug/sched/preempt` should show `voluntary`.
5. For the invalid-input case, boot with `preempt=bogus` and verify that while the `pr_warn` about "unsupported mode" appears, there is no `"Unknown kernel command line parameters"` warning from the init infrastructure.
