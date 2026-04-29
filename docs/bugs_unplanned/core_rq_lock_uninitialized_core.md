# Core: NULL rq->core dereference on uninitialized CPUs

**Commit:** `3c474b3239f12fe0b00d7e82481f36a1f31e79ab`
**Affected files:** kernel/sched/core.c, kernel/sched/sched.h
**Fixed in:** v5.14
**Buggy since:** v5.14-rc1 (introduced by commit 9edeaea1bc45 "sched: Core-wide rq->lock")

## Bug Description

When core scheduling (CONFIG_SCHED_CORE) is enabled, each CPU's run queue (`struct rq`) has a `core` pointer that determines which lock to use for the core-wide rq lock. The core scheduling feature groups SMT sibling CPUs so they share a single lock (the "core leader's" lock) to enable coordinated scheduling decisions across hyperthreads. The `rq->core` pointer is critical because `rq_lockp(rq)` dereferences it to find the actual spinlock to acquire.

The bug manifests when `cpu_possible_mask` differs from `cpu_online_mask` — that is, when the system has "possible" CPUs that have never been brought online. This situation occurs naturally on systems where the BIOS/firmware reports more CPUs than are physically present (e.g., certain HP ProLiant servers) or when the kernel is booted with `nr_cpus=` limiting online CPUs. In the original kernel initialization code (from commit 9edeaea1bc45), `rq->core` was initialized to `NULL` in `sched_init()`, and it was only set to a valid pointer by `sched_core_cpu_starting()` when a CPU actually comes online. CPUs that never come online therefore retain `rq->core == NULL`.

When core scheduling is activated (e.g., via `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, ...)`), certain code paths iterate over `for_each_possible_cpu()` and attempt to lock each CPU's run queue. For example, `online_fair_sched_group()` calls `rq_lock_irq(cpu_rq(i))` inside a `for_each_possible_cpu(i)` loop. With core scheduling active, `rq_lock_irq()` resolves to `raw_spin_rq_lock_nested()`, which calls `rq_lockp(rq)`. The `rq_lockp()` function returns `&rq->core->__lock` when core scheduling is enabled. Since `rq->core` is NULL for never-onlined CPUs, this produces a NULL pointer dereference.

The bug was discovered by Eugene Syromiatnikov on an HP ProLiant BL480c G1 where the BIOS reported 8 possible CPUs but only 4 were online. The reproducer involved calling `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, pgid, PIDTYPE_PGID, NULL)` after a child process called `setsid()`, which triggered autogroup creation via `sched_autogroup_create_attach()` → `online_fair_sched_group()`, leading to the crash.

## Root Cause

The root cause is twofold: an unsafe initialization value for `rq->core` and the absence of proper cleanup on CPU offline.

**Unsafe initialization:** In `sched_init()`, the original code set `rq->core = NULL` for every possible CPU. The design intent was that `sched_core_cpu_starting()` would set `rq->core` to a valid core leader pointer when the CPU comes online. However, this left a window where never-onlined CPUs had `rq->core == NULL`. The critical code path is:

```c
// In rq_lockp() with core scheduling enabled:
static inline raw_spinlock_t *rq_lockp(struct rq *rq)
{
    if (sched_core_enabled(rq))
        return &rq->core->__lock;  // NULL dereference when rq->core == NULL
    return &rq->__lock;
}
```

The function `raw_spin_rq_lock_nested()` implements a retry loop for the core scheduling case: it acquires `rq_lockp(rq)`, then verifies the lock hasn't changed (because the core topology might shift), retrying if needed. When `rq->core` is NULL, the very first `rq_lockp(rq)` call crashes.

**Missing CPU offline handling:** The original `sched_core_cpu_starting()` function checked `if (!core_rq)` — meaning it only ran the initialization logic if `rq->core` was NULL. There was no corresponding CPU offline handler to reset `rq->core` or transfer the core leader state. This meant that once a CPU came online and joined a core, taking it offline left stale `core` pointers across the SMT siblings. If the CPU later came back online in a different topology, the state could be inconsistent.

The `sched_core_cpu_starting()` function itself had a race condition: it accessed `rq->core` without holding any locks, and the assignment of `rq->core` for sibling CPUs was not done atomically with respect to other CPUs potentially reading `rq->core` at the same time.

## Consequence

The immediate consequence is a kernel NULL pointer dereference, producing an Oops and killing the offending process with SIGKILL. The specific crash trace from the bug report shows:

```
BUG: kernel NULL pointer dereference, address: 0000000000000000
RIP: 0010:do_raw_spin_trylock+0x5/0x40
Call Trace:
  _raw_spin_lock_nested+0x37/0x80
  raw_spin_rq_lock_nested+0x4b/0x80
  online_fair_sched_group+0x39/0x240
  sched_autogroup_create_attach+0x9d/0x170
  ksys_setsid+0xe6/0x110
```

The crash occurred when a userspace program called `setsid()`, which creates a new session and triggers autogroup creation. The autogroup code calls `online_fair_sched_group()`, which iterates `for_each_possible_cpu()` and locks each CPU's run queue. On the never-onlined CPU (with `rq->core == NULL`), this dereferences NULL.

Since the Oops is marked `[#67]` in the bug report, it means this crash had already occurred 67 times on the system, indicating it is highly reproducible once core scheduling is activated on affected hardware. The crash taints the kernel with `D` (DIE) and `W` (WARN), and the affected process is killed. Additionally, a secondary bug ("sleeping function called from invalid context") was observed in some test scenarios, suggesting further instability when running the full test suite.

## Fix Summary

The fix makes three key changes to ensure `rq->core` is always valid:

**1. Initialize `rq->core = rq` instead of `rq->core = NULL` in `sched_init()`:** This ensures that every CPU's run queue starts as its own "single-threaded core." Even if a CPU never comes online, `rq->core` points to itself, making `rq_lockp(rq)` return `&rq->__lock` — a valid spinlock. This eliminates the NULL pointer dereference entirely.

**2. Rework `sched_core_cpu_starting()` with proper locking:** The new implementation uses `sched_core_lock()`/`sched_core_unlock()` helpers that acquire all `rq->__lock` instances for the SMT mask atomically. It asserts `WARN_ON_ONCE(rq->core != rq)` (since every CPU now starts pointing to itself), then finds the core leader among already-online siblings and joins the onlining CPU to that core. The locking prevents races with concurrent `rq_lockp()` users.

**3. Add `sched_core_cpu_deactivate()` and `sched_core_cpu_dying()`:** The new `sched_core_cpu_deactivate()` function handles the case where the departing CPU is the core leader. It copies shared state (`core_task_seq`, `core_pick_seq`, `core_cookie`, `core_forceidle`, `core_forceidle_seq`) to a new leader and updates all siblings' `rq->core` pointers. The `sched_core_cpu_dying()` function resets the dying CPU's `rq->core` back to itself, restoring the initial "single-threaded core" state so the next online cycle starts clean.

This fix is correct and complete because it ensures the invariant that `rq->core` is always a valid, non-NULL pointer at all times, regardless of CPU online/offline state. The lock/unlock helpers (`sched_core_lock`/`sched_core_unlock`) bypass the `rq_lockp()` indirection by directly acquiring `rq->__lock`, avoiding the chicken-and-egg problem of needing a valid `rq->core` to lock the run queue.

## Triggering Conditions

The following conditions are all required to trigger this bug:

- **CONFIG_SCHED_CORE=y:** The kernel must be compiled with core scheduling support. Without this, `rq_lockp()` always returns `&rq->__lock` directly.
- **cpu_possible_mask ≠ cpu_online_mask:** There must be at least one CPU that is "possible" (reported by firmware/ACPI) but never brought online. This can happen due to BIOS reporting extra CPUs (as on the HP ProLiant BL480c G1 where possible=0-7 but online=0-3), or by booting with kernel parameters like `nr_cpus=` or `maxcpus=`.
- **Core scheduling activated:** The user must enable core scheduling, for example via `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, ...)`. This sets `sched_core_enabled(rq)` to true, causing `rq_lockp()` to go through `rq->core`.
- **Code path iterating for_each_possible_cpu() with rq locking:** A function must iterate all possible CPUs (not just online CPUs) and lock their run queues. The known trigger is `online_fair_sched_group()`, called from `sched_autogroup_create_attach()` during `setsid()`, or when creating fair scheduling groups for cgroups.

The bug is deterministic once these conditions are met — it crashes every time the code path is hit. No race condition or specific timing is needed. The reproducer from Eugene's report demonstrates this: fork a child, have it call `setsid()`, then call `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, pgid, PIDTYPE_PGID, NULL)`. On the second iteration, the child's `setsid()` crashes.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

**1. KERNEL VERSION TOO OLD:** The bug was introduced in v5.14-rc1 (commit 9edeaea1bc45 "sched: Core-wide rq->lock") and fixed before v5.14 was released (the fix commit 3c474b3239f1 is tagged as part of v5.14). kSTEP supports Linux v5.15 and newer only. Since the bug was already fixed before v5.15, no kernel version that kSTEP can run will contain this bug. The buggy window was limited to v5.14-rc1 through v5.14-rc6.

**2. Requires cpu_possible_mask ≠ cpu_online_mask:** Even if the kernel version were compatible, reproducing this bug requires that some "possible" CPUs never come online. In kSTEP's QEMU environment, all configured CPUs are brought online during boot. Simulating "possible but never online" CPUs would require either QEMU ACPI table manipulation to advertise phantom CPUs, or a kernel boot parameter like `maxcpus=` to limit online CPUs below the possible count. kSTEP does not provide APIs for configuring divergent possible/online CPU masks.

**3. Requires real userspace syscalls:** The known trigger path is `setsid()` → `sched_autogroup_create_attach()` → `online_fair_sched_group()`. kSTEP uses kernel threads (kthreads) rather than real userspace processes. Kthreads cannot call `setsid()` or trigger autogroup creation. While other paths through `online_fair_sched_group()` might exist (e.g., cgroup fair scheduling group operations), the fundamental blocker remains the kernel version.

**4. Requires core scheduling prctl():** The bug requires activating core scheduling via `prctl(PR_SCHED_CORE, ...)`, which is a userspace syscall. kSTEP cannot intercept or invoke userspace syscalls.

**What would need to change in kSTEP:** To support this class of bug, kSTEP would need: (a) support for pre-v5.15 kernels, (b) a mechanism to boot QEMU with more possible CPUs than online CPUs (e.g., `kstep_set_maxcpus(n)` to set the `maxcpus=` boot parameter), (c) an API to activate core scheduling (`kstep_core_sched_enable()` that internally calls the core scheduling toggle), and (d) a way to trigger autogroup creation or fair scheduling group onlining for all possible CPUs. However, since the bug is fixed in v5.14 and kSTEP starts at v5.15, these additions are moot for this specific bug.

**Alternative reproduction methods:** This bug can be reproduced outside kSTEP by: (1) running a v5.14-rcX kernel (rc1 through rc6) on real or virtual hardware where `cpu_possible_mask` has more CPUs than `cpu_online_mask` (e.g., QEMU with `-smp 4,maxcpus=8` and booting with `maxcpus=4`), (2) enabling core scheduling via `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, pgid, PIDTYPE_PGID, NULL)`, and (3) having a process call `setsid()`. The C reproducer from Eugene Syromiatnikov's bug report (included in the LKML thread) reliably triggers the crash.
