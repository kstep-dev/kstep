# Core: Off-by-one in preempt_model_str() misreports preempt_dynamic_none as "undef"

**Commit:** `3ebb1b6522392f64902b4e96954e35927354aa27`
**Affected files:** `kernel/sched/core.c`
**Fixed in:** v6.16-rc5
**Buggy since:** v6.15-rc1 (introduced by commit `8bdc5daaa01e` "sched: Add a generic function to return the preemption string")

## Bug Description

The `preempt_model_str()` function was introduced in v6.15-rc1 to provide a generic, architecture-independent way to report the current kernel preemption model as a human-readable string. This string appears in kernel backtraces, `/proc/version`, and diagnostic output. On kernels with `CONFIG_PREEMPT_DYNAMIC` enabled, the preemption model can be changed at boot time (via the `preempt=` kernel command line parameter) or at runtime (via the debugfs interface at `/sys/kernel/debug/sched/preempt`). The function is supposed to map the current `preempt_dynamic_mode` enum value to the corresponding string from the `preempt_modes[]` array: `{"none", "voluntary", "full", "lazy", NULL}`.

The bug is an off-by-one error in the comparison used to decide whether `preempt_dynamic_mode` represents a valid, initialized preemption mode. The code checks `preempt_dynamic_mode > 0` but the `preempt_dynamic_none` enum value is `0`, which is a perfectly valid initialized state. As a result, when the dynamic preemption mode is set to "none" (value 0), the function incorrectly falls through to the fallback string `"undef"`, producing `PREEMPT(undef)` instead of the correct `PREEMPT(none)`.

This affects any kernel compiled with `CONFIG_PREEMPT_DYNAMIC` where the preemption mode is set to "none", either by default (when `CONFIG_PREEMPT_NONE` is the base config), via boot parameter `preempt=none`, or via runtime debugfs write. The incorrect string propagates to all consumers of `preempt_model_str()`, including architecture-specific backtrace headers and version reporting.

## Root Cause

The `preempt_dynamic_mode` variable is an integer that tracks the current dynamic preemption mode. It is defined alongside an anonymous enum in `kernel/sched/core.c`:

```c
enum {
    preempt_dynamic_undefined = -1,
    preempt_dynamic_none,       // 0
    preempt_dynamic_voluntary,  // 1
    preempt_dynamic_full,       // 2
    preempt_dynamic_lazy,       // 3
};

int preempt_dynamic_mode = preempt_dynamic_undefined;
```

The variable starts at `preempt_dynamic_undefined` (-1) and is set to a valid mode during `preempt_dynamic_init()` at boot. When `CONFIG_PREEMPT_DYNAMIC` is not enabled, the variable is replaced by a macro: `#define preempt_dynamic_mode -1`.

The `preempt_model_str()` function constructs the preemption model string. For dynamic preemption, the relevant code is:

```c
if (IS_ENABLED(CONFIG_PREEMPT_DYNAMIC)) {
    seq_buf_printf(&s, "(%s)%s",
                   preempt_dynamic_mode > 0 ?
                   preempt_modes[preempt_dynamic_mode] : "undef",
                   brace ? "}" : "");
    return seq_buf_str(&s);
}
```

The condition `preempt_dynamic_mode > 0` is intended to distinguish between a valid initialized mode and the uninitialized/undefined state (-1). However, `preempt_dynamic_none` has the value `0`, which fails the `> 0` test. This means mode 0 ("none") is treated identically to mode -1 ("undefined"), and the function returns `"undef"` instead of indexing into `preempt_modes[0]` which is `"none"`.

The correct test is `preempt_dynamic_mode >= 0`, which correctly classifies mode 0 ("none") as a valid mode while still catching -1 ("undefined"). This ensures `preempt_modes[0]` returns `"none"` as intended.

Note that when `CONFIG_PREEMPT_DYNAMIC` is disabled, `preempt_dynamic_mode` is the macro `-1`, and the `>= 0` check correctly falls through to `"undef"`, so there is no regression for the non-dynamic case.

## Consequence

The observable impact is that the kernel misreports its preemption model string. When `preempt_dynamic_mode` is `preempt_dynamic_none` (0), the function returns `PREEMPT(undef)` instead of `PREEMPT(none)`. This string appears in:

1. **Kernel backtraces and oops/panic output**: Architecture-specific die handlers (e.g., x86, ARM64) call `preempt_model_str()` to include the preemption model in backtrace headers. A developer or automated crash analysis tool examining a crash dump would see `PREEMPT(undef)` and might incorrectly conclude the kernel's preemption mode was never initialized, leading to confusion or misdiagnosis.

2. **`/proc/version` and `uname -a` output**: The preemption model string is included in version information. Systems reporting `PREEMPT(undef)` instead of `PREEMPT(none)` may confuse administrators, monitoring tools, or configuration management systems that parse this field.

3. **Diagnostic and debugging contexts**: Any kernel subsystem or tool that calls `preempt_model_str()` to determine or display the current preemption model will receive incorrect information.

This bug does not cause crashes, hangs, data corruption, or incorrect scheduling behavior. It is purely an informational/cosmetic bug that causes misreporting of the kernel's preemption configuration. However, the misinformation can lead to wasted debugging time and incorrect conclusions about kernel configuration, which is why the fix was tagged for stable backport (`Cc: stable@vger.kernel.org`).

## Fix Summary

The fix is a single-character change in `kernel/sched/core.c` within the `preempt_model_str()` function. The comparison operator is changed from `>` (strictly greater than) to `>=` (greater than or equal to):

```c
// Before (buggy):
preempt_dynamic_mode > 0 ?
    preempt_modes[preempt_dynamic_mode] : "undef",

// After (fixed):
preempt_dynamic_mode >= 0 ?
    preempt_modes[preempt_dynamic_mode] : "undef",
```

This change ensures that `preempt_dynamic_none` (value 0) is correctly recognized as a valid initialized mode. The `preempt_modes[]` array is indexed as `preempt_modes[0]`, which returns `"none"`, producing the correct string `PREEMPT(none)`.

The fix is correct and complete because:
- All valid `preempt_dynamic_mode` values (0 through 3) are non-negative, so `>= 0` captures all of them.
- The only invalid/uninitialized value is `preempt_dynamic_undefined` (-1), which correctly fails the `>= 0` check and falls through to `"undef"`.
- When `CONFIG_PREEMPT_DYNAMIC` is disabled, the macro `#define preempt_dynamic_mode -1` also correctly fails the `>= 0` check.
- No other code paths or callers are affected; the fix is entirely contained within `preempt_model_str()`.

## Triggering Conditions

The bug requires the following precise conditions:

1. **CONFIG_PREEMPT_DYNAMIC must be enabled**: This kernel configuration option enables runtime-selectable preemption models. Without it, `preempt_dynamic_mode` is a compile-time macro set to -1, and the buggy code path is never reached (the `IS_ENABLED(CONFIG_PREEMPT_DYNAMIC)` guard prevents it).

2. **preempt_dynamic_mode must be set to preempt_dynamic_none (0)**: This occurs when:
   - The kernel is built with `CONFIG_PREEMPT_NONE=y` as the base preemption level (and `CONFIG_PREEMPT_DYNAMIC=y`), causing `preempt_dynamic_init()` to call `sched_dynamic_update(preempt_dynamic_none)`.
   - The kernel is booted with `preempt=none` on the command line, overriding the default.
   - A privileged user writes `"none"` to `/sys/kernel/debug/sched/preempt` at runtime.

3. **preempt_model_str() must be called**: The bug manifests only when the string is actually requested. This happens during backtrace printing, version string construction, or any other consumer of this function.

There are no race conditions, timing requirements, or multi-CPU dependencies. The bug is entirely deterministic: if the preemption mode is "none" and `preempt_model_str()` is called, the wrong string is always returned. Reproduction is 100% reliable under the above conditions.

The minimum topology is a single CPU. No cgroup configuration, task priorities, or workload characteristics are relevant — this bug is about string formatting, not scheduling behavior.

## Reproduce Strategy (kSTEP)

The bug can be reproduced without writing to any internal scheduler state. The previous approach directly assigned `preempt_dynamic_mode = 0`, but this is unnecessary because the kernel provides two fully public mechanisms to set the preemption mode to "none": the `preempt=none` boot parameter and the `/sys/kernel/debug/sched/preempt` debugfs file. The driver uses these public interfaces to reach the desired state, then reads `preempt_dynamic_mode` and calls `preempt_model_str()` purely for observation and verification.

### Step 1: Kernel Configuration

The kernel must be built with `CONFIG_PREEMPT_DYNAMIC=y` to enable runtime-selectable preemption modes, which is the prerequisite for the buggy code path inside `preempt_model_str()`. Additionally, `CONFIG_PREEMPT_BUILD=y` must be set (it is required for the `IS_ENABLED(CONFIG_PREEMPT_BUILD)` guard in `preempt_model_str()` to be entered). `CONFIG_DEBUG_FS=y` is needed for the debugfs-based runtime approach (writing to `/sys/kernel/debug/sched/preempt`). `CONFIG_KALLSYMS_ALL=y` should be enabled to ensure that `preempt_dynamic_mode` and `preempt_model_str` are accessible via kallsyms for read-only observation. No special preemption base config is strictly required since the driver will explicitly set the mode to "none" via public interfaces, but `CONFIG_PREEMPT_NONE=y` is an alternative if using the boot-parameter-only approach (see below).

### Step 2: Setting the Preemption Mode to "none" via Public Interfaces

There are two complementary strategies, and the driver should use both for robustness:

**Approach A — Boot parameter (`preempt=none`)**: The `run.py` launcher is invoked with the kernel command line parameter `preempt=none`. This causes `setup_preempt_mode()` (an `__init` function in `kernel/sched/core.c`) to record the requested mode, and then `preempt_dynamic_init()` (called during early boot via `sched_init()` → `preempt_dynamic_init()`) calls `sched_dynamic_update(preempt_dynamic_none)`, which sets `preempt_dynamic_mode = 0` through the kernel's own initialization path. By the time the kSTEP driver's `run()` callback is invoked, `preempt_dynamic_mode` is already 0. This is the simplest approach and requires zero runtime manipulation.

**Approach B — Debugfs write at runtime**: If the kernel was not booted with `preempt=none` (e.g., it booted with `preempt=voluntary` or `preempt=full`), the driver can switch the mode at runtime by writing `"none"` to `/sys/kernel/debug/sched/preempt`. This uses `kstep_write("/sys/kernel/debug/sched/preempt", "none")`, which triggers the kernel's `sched_dynamic_write()` function in `kernel/sched/core.c`. That function parses the string, finds the matching entry in `preempt_modes[]`, and calls `sched_dynamic_update(preempt_dynamic_none)`, which sets `preempt_dynamic_mode = 0` and reconfigures the static keys for preemption. This is a fully public kernel API — writing to debugfs files is a supported user-space interface, not an internal state manipulation. The driver can also use `kstep_write()` to set other modes (like "voluntary" or "full") and verify they work correctly as a control test.

Using approach A as the primary method is preferred because it guarantees the mode is "none" from early boot, meaning the bug would be visible in any `preempt_model_str()` call throughout the entire boot sequence. Approach B is useful for testing mode transitions at runtime and for verifying the bug manifests even when switching dynamically.

### Step 3: Driver Structure

The driver needs no tasks, no cgroups, no special topology, and only 1 CPU (the default CPU 0 where the driver runs). It is purely a function-call-and-check test. The key difference from the previous approach is that `preempt_dynamic_mode` is only read, never written:

```c
#include "../driver.h"
#include "../internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)

KSYM_IMPORT(preempt_dynamic_mode);
KSYM_IMPORT_TYPED(const char *(*)(void), preempt_model_str);

static void run(void) {
    const char *str;

    /*
     * Approach A: If booted with preempt=none, the mode is already 0.
     * Approach B: If not, write to debugfs to switch to "none" at runtime.
     */
    if (preempt_dynamic_mode != 0) {
        kstep_write("/sys/kernel/debug/sched/preempt", "none");
    }

    /* Verify the mode was set correctly (read-only observation) */
    if (preempt_dynamic_mode != 0) {
        kstep_fail("Failed to set preempt_dynamic_mode to none (got %d)",
                   preempt_dynamic_mode);
        return;
    }

    /* Call the function under test */
    str = preempt_model_str();

    /* Check if the string contains "none" (correct) or "undef" (buggy) */
    if (strstr(str, "none") && !strstr(str, "undef"))
        kstep_pass("preempt_model_str() correctly returns '%s' for mode none", str);
    else
        kstep_fail("preempt_model_str() returns '%s' for mode none, expected PREEMPT(none)", str);
}

KSTEP_DRIVER_DEFINE {
    .name = "core_preempt_none_str",
    .run = run,
};

#else
KSTEP_DRIVER_DEFINE {
    .name = "core_preempt_none_str",
};
#endif
```

### Step 4: Read-only Internal Access

`preempt_dynamic_mode` is imported via `KSYM_IMPORT(preempt_dynamic_mode)` and is used **strictly for read-only observation**. It is read in two places: (1) to check whether the boot parameter already set the mode to "none" (and if not, trigger the debugfs write), and (2) to verify that the debugfs write actually took effect before proceeding with the test. Neither read modifies any scheduler state.

`preempt_model_str()` is imported via `KSYM_IMPORT_TYPED` and called to obtain the string that is the subject of the bug. Calling this function is a pure observation — it constructs a string in a static buffer and returns it, with no side effects on scheduler state.

No other internal scheduler fields are accessed. The `cpu_rq()`, `cfs_rq`, or any other run-queue internals are not needed for this test since the bug is purely about string formatting, not scheduling behavior.

### Step 5: Test Operation Sequence

1. The driver's `run()` callback begins. If the kernel was booted with `preempt=none`, `preempt_dynamic_mode` is already 0.
2. If `preempt_dynamic_mode` is not 0 (e.g., kernel booted with a different default), the driver calls `kstep_write("/sys/kernel/debug/sched/preempt", "none")` to switch the mode via the kernel's own debugfs interface. This triggers `sched_dynamic_write()` → `sched_dynamic_update(preempt_dynamic_none)`, which updates `preempt_dynamic_mode` to 0 and reconfigures the static keys.
3. The driver reads `preempt_dynamic_mode` to confirm it is 0. If not, the debugfs write failed (unlikely but possible if debugfs is not mounted or the file does not exist), and the driver reports FAIL with a diagnostic message and exits early.
4. The driver calls `preempt_model_str()` to obtain the preemption model string.
5. The driver examines the returned string using `strstr()` to check for the expected substring `"none"` and the absence of the buggy substring `"undef"`.
6. On the **buggy kernel**: the string will contain `"undef"` (e.g., `"PREEMPT(undef)"` or `"PREEMPT_{RT,undef}"`). The driver reports `kstep_fail`.
7. On the **fixed kernel**: the string will contain `"none"` (e.g., `"PREEMPT(none)"` or `"PREEMPT_{RT,none}"`). The driver reports `kstep_pass`.

### Step 6: Topology and Task Setup

No special topology is needed. The default single-CPU or 2-CPU QEMU configuration suffices. No tasks need to be created. No cgroups are needed. No ticks or sleeps are required. The entire test runs in a single synchronous function call within the `run()` callback. The bug is deterministic and 100% reproducible whenever `preempt_dynamic_mode` is 0 and `preempt_model_str()` is called, so there is no need for timing-sensitive setup or repeated trials.

### Step 7: Expected Behavior

- **Buggy kernel (pre-fix, e.g., `3ebb1b65~1`)**: With `preempt_dynamic_mode` set to 0 via boot parameter or debugfs write, `preempt_model_str()` evaluates `preempt_dynamic_mode > 0` which is `0 > 0` → false, so it returns a string containing `"undef"` instead of indexing `preempt_modes[0]` ("none"). The driver detects the `"undef"` substring and reports FAIL.
- **Fixed kernel (post-fix, commit `3ebb1b65`)**: With the same mode, `preempt_model_str()` evaluates `preempt_dynamic_mode >= 0` which is `0 >= 0` → true, so it correctly indexes `preempt_modes[0]` and returns a string containing `"none"`. The driver detects the `"none"` substring and reports PASS.

### Step 8: kSTEP Configuration and Changes Required

`CONFIG_PREEMPT_DYNAMIC=y` must be enabled in the kernel config. `CONFIG_DEBUG_FS=y` must be enabled for the debugfs-based runtime approach (this is typically on by default in development configs). If using the boot-parameter approach exclusively, `CONFIG_DEBUG_FS` is not strictly required. The driver uses only existing kSTEP primitives: `KSYM_IMPORT` and `KSYM_IMPORT_TYPED` for read-only symbol access, `kstep_write()` for debugfs writes, and `kstep_pass`/`kstep_fail` for result reporting.

### Step 9: Build and Run Commands

```bash
# Build and run on buggy kernel (with preempt=none boot parameter)
./checkout_linux.py 3ebb1b6522392f64902b4e96954e35927354aa27~1 preempt_none_str_buggy
make linux LINUX_NAME=preempt_none_str_buggy
./run.py core_preempt_none_str --linux_name preempt_none_str_buggy --boot_args "preempt=none"
cat data/logs/latest.log  # Expect: FAIL

# Build and run on fixed kernel (with preempt=none boot parameter)
./checkout_linux.py 3ebb1b6522392f64902b4e96954e35927354aa27 preempt_none_str_fixed
make linux LINUX_NAME=preempt_none_str_fixed
./run.py core_preempt_none_str --linux_name preempt_none_str_fixed --boot_args "preempt=none"
cat data/logs/latest.log  # Expect: PASS
```

### Step 10: Why No Internal Writes Are Needed

The previous strategy directly assigned `preempt_dynamic_mode = 0` to force the mode to "none". This is unnecessary because the kernel provides two fully supported public interfaces to achieve the same effect: the `preempt=` boot parameter (processed by `setup_preempt_mode()` during early init) and the `/sys/kernel/debug/sched/preempt` debugfs file (processed by `sched_dynamic_write()`). Both ultimately call `sched_dynamic_update()`, which sets `preempt_dynamic_mode` and reconfigures the preemption static keys in a consistent manner. Using these public interfaces is not only cleaner but also more realistic — it exercises the exact code paths that real users would exercise, and it ensures that all related state (static keys, etc.) is kept consistent, unlike a bare assignment to `preempt_dynamic_mode` which would leave static keys in a potentially inconsistent state.
