# Core: NULL pointer dereference in sched_setaffinity() on non-SMP kernels

**Commit:** `5657c116783545fb49cd7004994c187128552b12`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.2-rc6
**Buggy since:** v6.2-rc3 (introduced by commit `9a5418bc48ba` "sched/core: Use kfree_rcu() in do_set_cpus_allowed()", merged in v6.2-rc3 window)

## Bug Description

When a Linux kernel is compiled without SMP support (`CONFIG_SMP=n`), calling the `sched_setaffinity()` syscall triggers a NULL pointer dereference (general protection fault). This occurs because the function `alloc_user_cpus_ptr()` returns NULL on non-SMP configs — there is no need for per-task user CPU affinity masks on a uniprocessor system — but the code unconditionally calls `cpumask_copy(user_mask, in_mask)` with the NULL `user_mask` pointer.

The bug was introduced by commit `9a5418bc48ba` ("sched/core: Use kfree_rcu() in do_set_cpus_allowed()"), which refactored `sched_setaffinity()` to allocate a separate `user_mask` via `alloc_user_cpus_ptr()`. The original error-checking logic was `if (IS_ENABLED(CONFIG_SMP) && !user_mask)`, which correctly detected allocation failure on SMP builds but allowed the NULL pointer to pass through on non-SMP builds because `IS_ENABLED(CONFIG_SMP)` evaluates to `0`, making the entire condition `false`. The subsequent `cpumask_copy()` call then dereferences the NULL pointer.

While this is not a practical concern for production systems — uniprocessor kernel configs are rare, and calling `sched_setaffinity()` on a single-CPU system is pointless — it was caught by the Intel kernel test robot running automated build and boot tests across diverse kernel configurations.

## Root Cause

The root cause lies in the interaction between the `alloc_user_cpus_ptr()` function and the error-checking logic in `sched_setaffinity()` at approximately line 8293 of `kernel/sched/core.c`.

On non-SMP kernel configurations, `alloc_user_cpus_ptr()` is defined as a stub that always returns `NULL`. This is intentional: the `user_cpus_ptr` field on `task_struct` does not exist when `CONFIG_SMP` is disabled, so there is no need to allocate a user affinity mask. The original code handled this correctly by checking `IS_ENABLED(CONFIG_SMP) && !user_mask` — if SMP is disabled, this condition short-circuits to `false` and the function proceeds. However, immediately after this check, the code unconditionally executes:

```c
cpumask_copy(user_mask, in_mask);
```

On SMP builds, `user_mask` is either a valid pointer (successful allocation) or the function has already jumped to `out_put_task` (failed allocation). On non-SMP builds, `user_mask` is always NULL because `alloc_user_cpus_ptr()` returns NULL, and the error check is bypassed because `IS_ENABLED(CONFIG_SMP)` is `0`. The `cpumask_copy()` call then performs a `memcpy()` into address `NULL`, causing a general protection fault.

The specific problematic code path before the fix is:

```c
user_mask = alloc_user_cpus_ptr(NUMA_NO_NODE);  // Returns NULL on !SMP
if (IS_ENABLED(CONFIG_SMP) && !user_mask) {      // false && true = false on !SMP
    retval = -ENOMEM;
    goto out_put_task;                             // NOT taken on !SMP
}
cpumask_copy(user_mask, in_mask);                 // CRASH: user_mask is NULL
```

The logic error is that the error-check conflates two distinct cases: (1) `alloc_user_cpus_ptr()` returned NULL because allocation failed (SMP build, out of memory), and (2) `alloc_user_cpus_ptr()` returned NULL because it is a no-op stub (non-SMP build). Only case (1) is an error; case (2) is expected behavior. But the `cpumask_copy()` call does not distinguish between these cases.

## Consequence

The immediate consequence is a general protection fault (GPF) — a kernel-level NULL pointer dereference — when any process calls `sched_setaffinity()` on a non-SMP kernel. This manifests as a kernel oops or panic, depending on the `panic_on_oops` sysctl setting. The faulting instruction is inside `cpumask_copy()` (which expands to `memcpy()`), with the destination pointer being NULL.

The impact is limited to non-SMP kernel configurations, which are uncommon in practice. Most distributions ship SMP-enabled kernels even for single-core systems. However, embedded systems, specialized appliances, and kernel test infrastructure may use `CONFIG_SMP=n` builds. The kernel test robot from Intel identified this issue during automated testing, indicating it affects CI/CD pipelines for kernel development.

On affected systems, any userspace program that calls `sched_setaffinity()` or `sched_setaffinity(2)` — including common tools like `taskset`, `numactl`, or any application using `pthread_setaffinity_np()` — would crash the kernel. This could be exploited as a local denial-of-service attack on vulnerable non-SMP kernels, since an unprivileged user can call `sched_setaffinity()` on their own process.

## Fix Summary

The fix restructures the conditional logic to properly handle both the SMP and non-SMP cases. Instead of checking `IS_ENABLED(CONFIG_SMP) && !user_mask` and then unconditionally copying, the fix checks whether `user_mask` is non-NULL first:

```c
user_mask = alloc_user_cpus_ptr(NUMA_NO_NODE);
if (user_mask) {
    cpumask_copy(user_mask, in_mask);    // Only copy if allocation succeeded
} else if (IS_ENABLED(CONFIG_SMP)) {
    retval = -ENOMEM;                     // SMP build: NULL means OOM
    goto out_put_task;
}
```

This correctly handles all three scenarios: (1) On SMP with successful allocation, `user_mask` is non-NULL, so the mask is copied. (2) On SMP with failed allocation, `user_mask` is NULL and `IS_ENABLED(CONFIG_SMP)` is true, so the function returns `-ENOMEM`. (3) On non-SMP, `user_mask` is NULL (by design) and `IS_ENABLED(CONFIG_SMP)` is false, so the function simply proceeds without copying, and the `affinity_context` struct receives `user_mask = NULL`, which is the expected value for non-SMP configs.

The fix also adds a clarifying comment explaining that `alloc_user_cpus_ptr()` intentionally returns NULL on non-SMP configs because `user_cpus_ptr`/`user_mask` is not used in that configuration. This makes the intent of the code clear to future developers and prevents similar confusion.

## Triggering Conditions

The following conditions must ALL be met to trigger this bug:

- **Kernel configuration:** The kernel must be compiled with `CONFIG_SMP=n` (uniprocessor configuration). This is the critical prerequisite — on any SMP-enabled kernel (which is the vast majority of kernels), the bug cannot be triggered.
- **Kernel version:** The kernel must include commit `9a5418bc48ba` (merged during v6.2 development cycle, approximately v6.2-rc3) but not yet include the fix commit `5657c116783545fb49cd7004994c187128552b12` (merged by v6.2-rc6). This gives a window of approximately 3 release candidates.
- **Syscall invocation:** A userspace process must call `sched_setaffinity()` (syscall number `__NR_sched_setaffinity`). This can be triggered by any user via `taskset`, `pthread_setaffinity_np()`, or a direct syscall. The specific PID and mask values do not matter — the crash occurs before the affinity mask is actually applied.
- **No special CPU topology or workload required:** Since the bug is a straightforward NULL pointer dereference in the syscall entry path, it triggers deterministically on every call to `sched_setaffinity()` on an affected kernel. There are no race conditions, timing dependencies, or workload characteristics needed. A simple `taskset -c 0 /bin/true` or equivalent is sufficient.

The reproduction is 100% reliable on an affected (non-SMP) kernel: every single call to `sched_setaffinity()` will crash.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

**1. The bug requires CONFIG_SMP=n, but kSTEP always builds with CONFIG_SMP=y.**

The entire bug is gated on a compile-time configuration option. When `CONFIG_SMP` is enabled (which kSTEP's kernel builds always use), `alloc_user_cpus_ptr()` is a real allocation function that returns a valid pointer (or NULL only on out-of-memory), and the original error-checking logic `IS_ENABLED(CONFIG_SMP) && !user_mask` correctly handles the OOM case. The buggy code path — where `alloc_user_cpus_ptr()` returns NULL by design (the non-SMP stub) and `cpumask_copy()` is called with that NULL pointer — is literally not compiled into an SMP kernel. There is no runtime condition, workload, or timing that can trigger the bug on an SMP kernel because the relevant code does not exist in the compiled binary.

This is fundamentally different from bugs that depend on the *number* of CPUs (which can be controlled via QEMU's `-smp` option). Even with `smp=1` in QEMU, the kernel is still compiled with `CONFIG_SMP=y`, and `alloc_user_cpus_ptr()` will still attempt a real allocation, following the SMP code path.

**2. The bug requires the sched_setaffinity() syscall, which kSTEP cannot invoke.**

Even if kSTEP could somehow build a non-SMP kernel, the bug is in the `sched_setaffinity()` function, which is a syscall entry point designed to be called from userspace. kSTEP is a kernel module that manages tasks via internal kernel APIs like `kstep_task_pin()` (which calls `set_cpus_allowed_ptr()` or similar internal functions, bypassing `sched_setaffinity()` entirely). kSTEP cannot intercept or make userspace syscalls. The function `sched_setaffinity()` performs additional work beyond what internal affinity-setting functions do, including the `alloc_user_cpus_ptr()` / `user_mask` handling that contains the bug.

**3. What would need to change in kSTEP to support this?**

Two fundamental changes would be required, neither of which is a minor extension:

- **Non-SMP kernel build support:** kSTEP's build system (`make linux`) would need to support building kernels with `CONFIG_SMP=n`. This is a major change because kSTEP's architecture assumes SMP (CPU topology setup, multi-CPU task pinning, load balancing observation, etc.). Most kSTEP APIs and callbacks would be meaningless or non-functional on a uniprocessor kernel.

- **Syscall invocation capability:** kSTEP would need a way to invoke `sched_setaffinity()` or similar syscalls from within a kernel module context. This could theoretically be done by calling the function directly (e.g., `KSYM_IMPORT(sched_setaffinity)` and calling it), but `sched_setaffinity()` expects a userspace calling context with proper credentials checking (`check_same_owner()`, `security_task_setscheduler()`), making it fragile to call from kernel context.

**4. Alternative reproduction methods outside kSTEP:**

The bug is trivially reproducible without kSTEP:

1. Build a kernel from the v6.2-rc3 to v6.2-rc5 range with `CONFIG_SMP=n` (e.g., using `make tinyconfig` or a custom `.config` with `# CONFIG_SMP is not set`).
2. Boot the kernel in QEMU with a single CPU: `qemu-system-x86_64 -kernel bzImage -append "console=ttyS0" -initrd initramfs.cpio -nographic`
3. From any userspace process, run: `taskset -c 0 /bin/true` (or call `sched_setaffinity(0, sizeof(cpu_set_t), &mask)` directly).
4. The kernel will immediately oops with a general protection fault in `cpumask_copy()`.

Alternatively, the Intel kernel test robot's automated boot testing with `CONFIG_SMP=n` configurations is how this bug was originally discovered — it triggers during any normal system operation where affinity is set.

**5. Summary:**

This bug belongs in `drivers_unplanned` because the buggy code is not compiled into SMP kernels. Since kSTEP exclusively builds SMP kernels, the NULL pointer dereference in the non-SMP stub path of `alloc_user_cpus_ptr()` simply does not exist in any kernel that kSTEP can produce. This is a compile-time configuration issue, not a runtime scheduling behavior issue that could be triggered through task management or workload orchestration.
