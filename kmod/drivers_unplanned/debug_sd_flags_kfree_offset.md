# Debug: Memory Corruption from kfree of Offset Pointer in sd_ctl_doflags

**Commit:** `8d4d9c7b4333abccb3bf310d76ef7ea2edb9828f`
**Affected files:** kernel/sched/debug.c
**Fixed in:** v5.10-rc4
**Buggy since:** v5.10-rc1 (commit `5b9f8ff7b320` — "sched/debug: Output SD flag names rather than their values")

## Bug Description

The function `sd_ctl_doflags()` in `kernel/sched/debug.c` is the proc handler for reading the `flags` file under `/proc/sys/kernel/sched_domain/cpu*/domain*/flags`. It was introduced in commit `5b9f8ff7b320` to output human-readable sched domain flag names instead of raw numeric values. When userspace reads this file, `sd_ctl_doflags()` dynamically allocates a buffer with `kcalloc()`, populates it with the flag names, then copies the appropriate portion to userspace based on the current file position (`*ppos`).

The bug manifests when the file is read in multiple small sequential reads rather than a single large read. For example, if userspace reads only 3 bytes at a time, the first read works correctly. However, on subsequent reads, the function advances the `tmp` pointer by `*ppos` bytes into the allocated buffer (`tmp += *ppos`), then later calls `kfree(tmp)` on this advanced pointer rather than the original allocation base. This passes an interior pointer to the SLUB allocator, which is undefined behavior and causes heap corruption.

The bug was reported by Jeff Bastian at Red Hat and also detected by Colin Ian King using `stress-ng --procfs 0` on a v5.10-rc1 kernel. The stress-ng procfs stressor performs rapid, randomized reads of all `/proc` and `/sys` files with varying buffer sizes, which reliably triggers the small-read pattern that exposes this bug.

## Root Cause

In the original buggy code, the function `sd_ctl_doflags()` uses a single pointer variable `tmp` for both the allocation and the data manipulation:

```c
tmp = kcalloc(data_size + 1, sizeof(*tmp), GFP_KERNEL);
if (!tmp)
    return -ENOMEM;

for_each_set_bit(idx, &flags, __SD_FLAG_CNT) {
    char *name = sd_flag_debug[idx].name;
    len += snprintf(tmp + len, strlen(name) + 2, "%s ", name);
}

tmp += *ppos;   /* PROBLEM: advances tmp past the allocation start */
len -= *ppos;
/* ... memcpy from tmp to buffer ... */
kfree(tmp);     /* BUG: frees interior pointer, not original allocation */
```

On the first read (`*ppos == 0`), `tmp += *ppos` is a no-op, so `kfree(tmp)` correctly frees the original allocation. But on subsequent reads, `*ppos` is non-zero (it accumulates the number of bytes already returned). The line `tmp += *ppos` advances `tmp` to point into the middle of the allocated SLUB object. When `kfree(tmp)` is then called with this interior pointer, the SLUB allocator receives a pointer that does not match any known object header.

The SLUB allocator's `__check_heap_object()` detects that the pointer offset within the SLUB object is invalid during the `usercopy` check (CONFIG_HARDENED_USERCOPY). It finds that the requested copy would extend beyond the SLUB object boundary (e.g., offset 127 from a `kmalloc-128` object with 3 bytes remaining), triggering the `usercopy_abort()` which hits `BUG()` at `mm/usercopy.c:99`. Without hardened usercopy, the `kfree()` of the interior pointer would silently corrupt the SLUB freelist, leading to unpredictable memory corruption.

The root cause is simply that the original code reused the same `tmp` variable for both tracking the allocation (for `kfree`) and computing the read offset (for `memcpy`). This is a classic use-after-modification pattern where the pointer needed for deallocation is lost.

## Consequence

The immediate observable consequence is a kernel BUG/oops when CONFIG_HARDENED_USERCOPY is enabled. The kernel crashes with:

```
usercopy: Kernel memory exposure attempt detected from SLUB object 'kmalloc-128' (offset 127, size 3)!
kernel BUG at mm/usercopy.c:99!
invalid opcode: 0000 [#1] SMP PTI
```

The call trace shows `__check_heap_object → __check_object_size → proc_sys_call_handler → new_sync_read → vfs_read → ksys_read`, confirming the bug is triggered by a standard userspace `read()` syscall on the proc file.

Without CONFIG_HARDENED_USERCOPY, the consequence is silent SLUB heap corruption. The `kfree()` of an interior pointer corrupts the SLUB freelist metadata, which can lead to: (1) use-after-free vulnerabilities if the corrupted freelist causes SLUB to return already-in-use memory; (2) kernel panics from corrupted slab state on subsequent allocations or frees; (3) potential security vulnerabilities if an attacker can control the read pattern to corrupt specific heap objects. The bug is trivially triggerable by any unprivileged user who can read `/proc/sys/kernel/sched_domain/` files, making it a local denial-of-service vulnerability.

## Fix Summary

The fix introduces a second pointer variable `buf` to track the original allocation, while `tmp` is used solely as a temporary pointer for computing the read offset:

```c
char *tmp, *buf;
/* ... */
buf = kcalloc(data_size + 1, sizeof(*buf), GFP_KERNEL);
if (!buf)
    return -ENOMEM;

for_each_set_bit(idx, &flags, __SD_FLAG_CNT) {
    char *name = sd_flag_debug[idx].name;
    len += snprintf(buf + len, strlen(name) + 2, "%s ", name);
}

tmp = buf + *ppos;   /* tmp points into buf at the read offset */
len -= *ppos;
/* ... memcpy from tmp to buffer ... */
kfree(buf);          /* CORRECT: frees the original allocation */
```

Now `buf` always holds the original `kcalloc()` return value and is never modified. The `snprintf` calls write into `buf + len` (using `buf` directly), and `tmp` is set to `buf + *ppos` as a computed value (assignment, not increment). The `kfree(buf)` at the end always frees the correct pointer regardless of the value of `*ppos`. This is a minimal, surgical fix that changes only the pointer management without altering the logic of flag name formatting or data copying.

## Triggering Conditions

- **Kernel version**: v5.10-rc1 through v5.10-rc3 (commits `5b9f8ff7b320` through just before `8d4d9c7b4333`).
- **Kernel configuration**: CONFIG_SMP must be enabled (for sched domains to exist). CONFIG_SCHED_DEBUG must be enabled (for the `/proc/sys/kernel/sched_domain/` hierarchy to be registered). CONFIG_HARDENED_USERCOPY makes the crash deterministic; without it, silent corruption occurs.
- **Trigger action**: A userspace process must read `/proc/sys/kernel/sched_domain/cpuN/domainM/flags` in multiple small reads. The first read must not consume the entire content, so the second read has a non-zero `*ppos`. Using a buffer size smaller than the total flags string length (typically ~100-200 bytes) will trigger this.
- **Reproduction command**: `stress-ng --procfs 0` reliably triggers the bug because it reads proc files with randomized small buffer sizes.
- **No special hardware or topology required**: Any SMP system with at least one sched domain will have the `flags` file. The bug is deterministic once a multi-read pattern occurs.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **Kernel version too old (pre-v5.15)**: The bug was introduced in v5.10-rc1 (commit `5b9f8ff7b320`) and fixed in v5.10-rc4 (commit `8d4d9c7b4333`). It never existed in any kernel v5.15 or newer. kSTEP requires Linux v5.15 as its minimum supported version, so there is no kernel version where this bug exists that kSTEP can run on.

2. **Requires userspace procfs reads**: Even if the kernel version were supported, the bug is in the proc handler `sd_ctl_doflags()` which is invoked only through the VFS `read()` syscall path on `/proc/sys/kernel/sched_domain/cpuN/domainM/flags`. kSTEP operates as a kernel module and cannot intercept or issue userspace syscalls. There is no kSTEP API for reading proc/sysfs files (only `kstep_sysctl_write()` exists for writing). The bug cannot be triggered from kernel space.

3. **Not a scheduling behavior bug**: This is a memory management bug (invalid `kfree` of interior pointer) in the debug/procfs interface code. It does not affect any scheduling decisions, task placement, load balancing, or timing behavior. Even if kSTEP could somehow trigger the read, the bug would manifest as SLUB corruption or a BUG_ON in `mm/usercopy.c`, neither of which relates to schedulable task behavior that kSTEP can observe.

4. **What would need to change in kSTEP**: To support this class of bug, kSTEP would need: (a) a `kstep_sysctl_read(path, buffer, len, ppos)` API to read proc/sysfs files with controlled buffer sizes and positions — this would be a fundamental addition to the framework; (b) support for kernels older than v5.15 — this would require rebuilding the entire QEMU/module infrastructure.

5. **Alternative reproduction**: The bug is trivially reproducible outside kSTEP by running `stress-ng --procfs 0` on a v5.10-rc1 through v5.10-rc3 kernel, or by writing a simple C program that opens `/proc/sys/kernel/sched_domain/cpu0/domain0/flags` and reads it with a 1-byte buffer in a loop. The `dd` command can also trigger it: `dd if=/proc/sys/kernel/sched_domain/cpu0/domain0/flags bs=1`.
