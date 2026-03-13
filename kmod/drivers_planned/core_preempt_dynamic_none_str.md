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

The bug can be reproduced with a straightforward kSTEP driver that manipulates the `preempt_dynamic_mode` variable and calls `preempt_model_str()` to verify the output. Here is the detailed plan:

### Step 1: Kernel Configuration

The kernel must be built with `CONFIG_PREEMPT_DYNAMIC=y`. This is typically enabled on modern kernel configs. If it is not enabled by default in kSTEP's kernel config, it needs to be added. Additionally, `CONFIG_PREEMPT_BUILD=y` must be set (it is a prerequisite for the code path that calls `seq_buf_printf` with the preemption mode string). The `CONFIG_KALLSYMS_ALL=y` option should be enabled (it usually is) to ensure `preempt_dynamic_mode` is accessible via kallsyms.

### Step 2: Driver Structure

The driver needs no tasks, no cgroups, no special topology, and only 1 CPU. It is purely a function-call-and-check test:

```c
#include "../driver.h"
#include "../internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)

KSYM_IMPORT(preempt_dynamic_mode);
KSYM_IMPORT_TYPED(const char *(*)(void), preempt_model_str);

static void run(void) {
    const char *str;
    int saved_mode;

    /* Save original mode */
    saved_mode = preempt_dynamic_mode;

    /* Set to preempt_dynamic_none (0) to trigger the bug */
    preempt_dynamic_mode = 0;

    /* Call the function under test */
    str = preempt_model_str();

    /* Check if the string contains "none" (correct) or "undef" (buggy) */
    if (strstr(str, "none") && !strstr(str, "undef"))
        kstep_pass("preempt_model_str() correctly returns '%s' for mode none", str);
    else
        kstep_fail("preempt_model_str() returns '%s' for mode none, expected PREEMPT(none)", str);

    /* Restore original mode */
    preempt_dynamic_mode = saved_mode;
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

### Step 3: Variable Access

`preempt_dynamic_mode` is a global (non-static) integer variable declared `extern` in `kernel/sched/sched.h` and defined in `kernel/sched/core.c`. It is accessible via `KSYM_IMPORT(preempt_dynamic_mode)`. The `preempt_model_str()` function is also a global non-static function. It can be imported via `KSYM_IMPORT_TYPED` with the appropriate function pointer type.

### Step 4: Test Operation Sequence

1. Save the current value of `preempt_dynamic_mode` (to restore later).
2. Set `preempt_dynamic_mode = 0` (which is `preempt_dynamic_none`).
3. Call `preempt_model_str()`.
4. Examine the returned string using `strstr()`.
5. On the **buggy kernel**: the string will contain `"undef"` (e.g., `"PREEMPT(undef)"`). Report `kstep_fail`.
6. On the **fixed kernel**: the string will contain `"none"` (e.g., `"PREEMPT(none)"`). Report `kstep_pass`.
7. Restore `preempt_dynamic_mode` to its original value.

### Step 5: Topology and Task Setup

No special topology is needed. The default single-CPU or 2-CPU QEMU configuration suffices. No tasks need to be created. No cgroups are needed. No ticks or sleeps are required. The entire test runs in a single synchronous function call within the `run()` callback.

### Step 6: Expected Behavior

- **Buggy kernel (pre-fix, e.g., `3ebb1b65~1`)**: `preempt_model_str()` returns a string containing `"undef"` when `preempt_dynamic_mode == 0`. The driver reports FAIL.
- **Fixed kernel (post-fix, commit `3ebb1b65`)**: `preempt_model_str()` returns a string containing `"none"` when `preempt_dynamic_mode == 0`. The driver reports PASS.

### Step 7: kSTEP Changes Required

If `CONFIG_PREEMPT_DYNAMIC` is not already enabled in kSTEP's kernel config, it needs to be added. This is a minor configuration change, not a fundamental kSTEP limitation. The driver itself uses only existing kSTEP primitives (`KSYM_IMPORT`, `KSYM_IMPORT_TYPED`, `kstep_pass`, `kstep_fail`).

### Step 8: Build and Run Commands

```bash
# Build and run on buggy kernel
./checkout_linux.py 3ebb1b6522392f64902b4e96954e35927354aa27~1 preempt_none_str_buggy
make linux LINUX_NAME=preempt_none_str_buggy
./run.py core_preempt_none_str --linux_name preempt_none_str_buggy
cat data/logs/latest.log  # Expect: FAIL

# Build and run on fixed kernel
./checkout_linux.py 3ebb1b6522392f64902b4e96954e35927354aa27 preempt_none_str_fixed
make linux LINUX_NAME=preempt_none_str_fixed
./run.py core_preempt_none_str --linux_name preempt_none_str_fixed
cat data/logs/latest.log  # Expect: PASS
```
