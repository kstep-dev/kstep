# RT: sysctl_sched_rr_timeslice Initial Value Rounding Error

**Commit:** `c7fcb99877f9f542c918509b2801065adcaf46fa`
**Affected files:** kernel/sched/rt.c
**Fixed in:** v6.6-rc1
**Buggy since:** v4.11-rc1 (commit 975e155ed873 "sched/rt: Show the 'sched_rr_timeslice' SCHED_RR timeslice tuning knob in milliseconds")

## Bug Description

The static initializer for the `sysctl_sched_rr_timeslice` variable in `kernel/sched/rt.c` contains an integer arithmetic rounding error that causes the value exposed via `/proc/sys/kernel/sched_rr_timeslice_ms` to be incorrect when the kernel is compiled with `CONFIG_HZ_300=y`. The sysctl reports 90 ms instead of the correct 100 ms for the default SCHED_RR timeslice.

The bug was introduced in commit 975e155ed873, which added a `sysctl_sched_rr_timeslice` variable to properly display the RR timeslice in milliseconds rather than raw jiffies. The original intent was to fix confusion where the sysctl value appeared in jiffies (e.g., showing 10 instead of 100 when HZ=100). However, the formula used for the initial value performs integer division before multiplication, causing truncation when `MSEC_PER_SEC` (1000) is not evenly divisible by `HZ`.

The internal scheduler variable `sched_rr_timeslice` (stored in jiffies as `RR_TIMESLICE`) is always correct — only the sysctl display variable is wrong. This means actual SCHED_RR scheduling behavior is unaffected, but userspace tools and tests that read `/proc/sys/kernel/sched_rr_timeslice_ms` see an inconsistent value. The bug was discovered by the LTP (Linux Test Project) test `sched_rr_get_interval01`, which compares the `sched_rr_get_interval()` syscall return value against the sysctl file and detected the mismatch.

## Root Cause

The buggy initializer in `kernel/sched/rt.c` line 28 is:

```c
static int sysctl_sched_rr_timeslice = (MSEC_PER_SEC / HZ) * RR_TIMESLICE;
```

where `MSEC_PER_SEC` is 1000, `HZ` is the kernel tick rate, and `RR_TIMESLICE` is defined as `(100 * HZ / 1000)` (i.e., 100ms worth of jiffies).

The problem is that in C integer arithmetic, `(MSEC_PER_SEC / HZ)` is evaluated first. When `HZ=300`, this computes `1000 / 300 = 3` (truncated from 3.333...). Then `3 * RR_TIMESLICE` where `RR_TIMESLICE = (100 * 300 / 1000) = 30` gives `3 * 30 = 90`. The correct answer is 100 ms.

The root issue is that 1000 is not evenly divisible by 300, so the integer division `1000 / 300` truncates the fractional part (0.333...), losing ~10% of the value. This truncation is then amplified by the subsequent multiplication. The formula works correctly for `HZ=100` (1000/100=10, 10*10=100), `HZ=250` (1000/250=4, 4*25=100), and `HZ=1000` (1000/1000=1, 1*100=100) because those values divide 1000 evenly. Only `CONFIG_HZ_300` triggers the issue.

The key insight is that the order of operations matters in integer arithmetic. By performing the multiplication before the division (i.e., `(MSEC_PER_SEC * RR_TIMESLICE) / HZ`), the intermediate product `1000 * 30 = 30000` is large enough that the final division `30000 / 300 = 100` produces the exact correct result with no truncation.

## Consequence

The observable impact is that reading `/proc/sys/kernel/sched_rr_timeslice_ms` on a `CONFIG_HZ_300=y` kernel returns 90 instead of 100. This is a 10% error in the reported SCHED_RR timeslice value.

The primary consequence is that userspace applications and testing frameworks that rely on the sysctl value to determine the SCHED_RR timeslice see an incorrect value. The LTP test `sched_rr_get_interval01` fails with:
```
sched_rr_get_interval01.c:72: TFAIL: /proc/sys/kernel/sched_rr_timeslice_ms != 100 got 90
```
The `sched_rr_get_interval()` syscall correctly reports ~100ms (99999990ns), but the sysctl file reports 90ms, creating an inconsistency that breaks compliance testing and can confuse system administrators or monitoring tools.

Importantly, this bug does NOT affect actual SCHED_RR scheduling behavior. The internal `sched_rr_timeslice` variable (in jiffies) is initialized correctly to `RR_TIMESLICE` and is the one used by the scheduler to determine when to preempt round-robin tasks. The `sysctl_sched_rr_timeslice` variable is purely for sysctl display and write-back purposes. However, if an administrator reads the sysctl value, believes it is correct (90ms), and then writes it back without modification (e.g., during a configuration save/restore), the actual timeslice would be changed to `msecs_to_jiffies(90)` which is 27 jiffies at HZ=300, rather than the correct 30 jiffies — a subtle secondary degradation.

## Fix Summary

The fix changes a single line in `kernel/sched/rt.c`, reordering the arithmetic in the static initializer:

```c
// Before (buggy):
static int sysctl_sched_rr_timeslice = (MSEC_PER_SEC / HZ) * RR_TIMESLICE;

// After (fixed):
static int sysctl_sched_rr_timeslice = (MSEC_PER_SEC * RR_TIMESLICE) / HZ;
```

By performing the multiplication `MSEC_PER_SEC * RR_TIMESLICE` first, the intermediate result is `1000 * 30 = 30000` (for HZ=300). This larger intermediate value is then divided by HZ: `30000 / 300 = 100`, yielding the exact correct millisecond value. There is no risk of integer overflow here because `MSEC_PER_SEC * RR_TIMESLICE` is at most `1000 * 100 = 100000` (when HZ=1000), well within the range of a 32-bit `int`.

The fix is correct and complete because it ensures that the sysctl display variable matches `jiffies_to_msecs(RR_TIMESLICE)` for all supported HZ values (100, 250, 300, 1000). The companion patch (commit 2 in the same series) also fixes the `sched_rr_handler()` to reset `sysctl_sched_rr_timeslice` to `jiffies_to_msecs(RR_TIMESLICE)` when a user writes a value ≤ 0 (which is the "reset to default" mechanism), ensuring consistency after reset operations as well.

## Triggering Conditions

The bug requires the following precise conditions:

1. **Kernel configuration**: The kernel must be compiled with `CONFIG_HZ_300=y`. This is the only standard HZ value where `MSEC_PER_SEC` (1000) is not evenly divisible by HZ. The other standard options (`CONFIG_HZ_100`, `CONFIG_HZ_250`, `CONFIG_HZ_1000`) all divide 1000 evenly and do not trigger the bug.

2. **CONFIG_SYSCTL**: Must be enabled (which it is in virtually all kernel configurations). The `sysctl_sched_rr_timeslice` variable only exists under `#ifdef CONFIG_SYSCTL`.

3. **Reading the sysctl**: The bug manifests when reading `/proc/sys/kernel/sched_rr_timeslice_ms` — the initial value will be 90 instead of 100.

4. **No prior writes**: The bug is in the *initial* value. If a user has written to the sysctl since boot, the value will reflect whatever was written (via `sched_rr_handler()`), not the buggy initializer. So the bug is observable only before the first write to this sysctl after boot.

There are no race conditions, concurrency requirements, or timing dependencies. The bug is deterministic and 100% reproducible on any `CONFIG_HZ_300=y` kernel. It is a pure compile-time arithmetic error.

## Reproduce Strategy (kSTEP)

The reproduction strategy requires building the kernel with `CONFIG_HZ_300=y` and verifying that the sysctl value is incorrect. Here is a detailed plan for a kSTEP driver:

**1. Kernel Build Configuration:**
The kernel must be built with `CONFIG_HZ_300=y` instead of kSTEP's default `CONFIG_HZ_1000=y`. This can be achieved using the `KSTEP_EXTRA_CONFIG` mechanism in the Makefile. Create a config fragment file (e.g., `linux/config.hz300`) containing:
```
# CONFIG_HZ_1000 is not set
CONFIG_HZ_300=y
CONFIG_HZ=300
```
Then build with: `make linux LINUX_NAME=<name> KSTEP_EXTRA_CONFIG=linux/config.hz300`

**2. Task Setup:**
No special task setup is required. The bug is in a static initializer, not in runtime scheduling behavior. The driver only needs to run initialization code to check the value. A single task or even just the driver's init function is sufficient.

**3. Detecting the Bug — Approach A (Recommended): Read proc file from kernel space:**
The driver can use kernel file I/O to read `/proc/sys/kernel/sched_rr_timeslice_ms`:
```c
struct file *f;
char buf[32];
loff_t pos = 0;
ssize_t ret;

f = filp_open("/proc/sys/kernel/sched_rr_timeslice_ms", O_RDONLY, 0);
if (!IS_ERR(f)) {
    ret = kernel_read(f, buf, sizeof(buf) - 1, &pos);
    buf[ret] = '\0';
    filp_close(f, NULL);
    int sysctl_val;
    kstrtoint(buf, 10, &sysctl_val);
    int expected = jiffies_to_msecs(RR_TIMESLICE);
    if (sysctl_val != expected)
        kstep_fail("sysctl_sched_rr_timeslice_ms = %d, expected %d", sysctl_val, expected);
    else
        kstep_pass("sysctl_sched_rr_timeslice_ms = %d matches expected %d", sysctl_val, expected);
}
```

**4. Detecting the Bug — Approach B: Import non-static sched_rr_timeslice:**
The `sched_rr_timeslice` variable (the actual timeslice in jiffies, not the sysctl one) is non-static and can be imported via `KSYM_IMPORT(sched_rr_timeslice)`. The driver can convert it to milliseconds and compare against the proc file:
```c
KSYM_IMPORT(sched_rr_timeslice);
// In driver:
int actual_ms = jiffies_to_msecs(*KSYM_sched_rr_timeslice);
// Compare with value read from /proc/sys/kernel/sched_rr_timeslice_ms
```

**5. Version Guard:**
The driver should be guarded with `#if LINUX_VERSION_CODE` to target kernels between v4.11 (975e155ed873) and v6.5 (before c7fcb99877f9). Since kSTEP supports v5.15+, the lower bound is effectively v5.15.

**6. Expected Behavior:**
- **Buggy kernel (CONFIG_HZ_300, pre-fix):** `/proc/sys/kernel/sched_rr_timeslice_ms` reads 90. `jiffies_to_msecs(RR_TIMESLICE)` returns 100. Mismatch → `kstep_fail()`.
- **Fixed kernel (CONFIG_HZ_300, post-fix):** `/proc/sys/kernel/sched_rr_timeslice_ms` reads 100. `jiffies_to_msecs(RR_TIMESLICE)` returns 100. Match → `kstep_pass()`.

**7. kSTEP Extensions Needed:**
No fundamental kSTEP extensions are needed. The driver uses standard kernel file I/O (`filp_open`/`kernel_read`) which is available to any kernel module. The only non-standard requirement is the custom kernel config (`CONFIG_HZ_300=y`), which is supported via `KSTEP_EXTRA_CONFIG`. Optionally, a `kstep_sysctl_read()` helper could be added to the framework for convenience, but it is not strictly necessary.

**8. QEMU Configuration:**
No special QEMU configuration is required. A single CPU with default RAM is sufficient. The number of CPUs and topology are irrelevant since this is a static initialization bug, not a scheduling behavior bug.

**9. Caveats:**
- Using `CONFIG_HZ_300` instead of `CONFIG_HZ_1000` changes tick granularity, which may affect other kSTEP functionality that depends on 1ms ticks. This driver should be run in isolation with its own kernel build.
- The `sysctl_sched_rr_timeslice` variable is `static` in kernels from around v6.2+ (after the sysctl table restructuring). In earlier kernels (v5.15–v6.1), it may be non-static and directly importable via `KSYM_IMPORT`, which would simplify the driver.
- The driver should verify `CONFIG_HZ == 300` at runtime (via the `HZ` macro) to confirm the test is meaningful. If HZ is not 300, the test should be skipped since the bug only manifests with that value.
