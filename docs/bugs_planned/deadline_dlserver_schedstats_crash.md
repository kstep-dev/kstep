# Deadline: dl_server schedstats BUG_ON Crash

**Commit:** `9c602adb799e72ee537c0c7ca7e828c3fe2acad6`
**Affected files:** `kernel/sched/deadline.c`
**Fixed in:** v6.12-rc1
**Buggy since:** v6.8-rc1 (commit `63ba8422f876` "sched/deadline: Introduce deadline servers")

## Bug Description

When the kernel is compiled with `CONFIG_SCHEDSTATS=y` and scheduler statistics are enabled at runtime (via `/proc/sys/kernel/sched_schedstats`), any CFS task enqueue operation triggers a fatal `BUG_ON` in `dl_task_of()`. This happens because the deadline server infrastructure (`dl_server`), introduced in v6.8, uses internal `sched_dl_entity` objects that are not embedded in any `task_struct`. When the schedstats code path tries to access the task's statistics for a dl_server entity, it calls `dl_task_of()` which performs a `container_of()` to recover the parent `task_struct`. However, since the dl_server's `sched_dl_entity` is not part of a task, this produces a garbage pointer, and the `BUG_ON(dl_server(dl_se))` guard at line 67 of `deadline.c` catches this invalid access and panics.

The deadline server mechanism provides bandwidth guarantees for CFS tasks by using an internal SCHED_DEADLINE entity to "serve" the CFS runqueue. When `enqueue_task_fair()` is called (e.g., during `wake_up_new_task()`, task wakeup, or fork), it calls `dl_server_start()` to activate the per-rq deadline server. This server entity has `dl_se->dl_server = 1` to distinguish it from regular SCHED_DEADLINE tasks. The server is then enqueued via `enqueue_dl_entity()`, which calls `update_stats_enqueue_dl()` to update scheduler statistics. The schedstats functions were not updated to handle dl_server entities when the deadline server infrastructure was introduced.

The original report comes from running memcached on an Ampere Altra system (ARM64) with schedstats enabled. The crash manifests as `kernel BUG at kernel/sched/deadline.c:63!` followed by a kernel panic, making the system completely unresponsive. Multiple CPUs hit the bug simultaneously since every CPU's CFS task enqueue path triggers `dl_server_start()`.

## Root Cause

The root cause is in the function `__schedstats_from_dl_se()` which, in the buggy version, unconditionally calls `dl_task_of(dl_se)`:

```c
// Buggy version
static inline struct sched_statistics *
__schedstats_from_dl_se(struct sched_dl_entity *dl_se)
{
    return &dl_task_of(dl_se)->stats;
}
```

The `dl_task_of()` function is defined as:

```c
static inline struct task_struct *dl_task_of(struct sched_dl_entity *dl_se)
{
    BUG_ON(dl_server(dl_se));
    return container_of(dl_se, struct task_struct, dl);
}
```

The `BUG_ON(dl_server(dl_se))` assertion fires because the dl_server entity has `dl_se->dl_server = 1`. This entity is not part of a `task_struct` — it is a standalone `sched_dl_entity` embedded in the per-rq CFS infrastructure. Calling `container_of()` on it would produce a bogus pointer to non-existent task memory, so the `BUG_ON` correctly catches this misuse.

The call chain that triggers the crash is:

1. `dl_server_start(dl_se)` — activates the per-rq deadline server
2. `enqueue_dl_entity(dl_se, ENQUEUE_WAKEUP)` — enqueues the server entity onto the DL runqueue
3. `update_stats_enqueue_dl(dl_rq, dl_se, flags)` — checks `schedstat_enabled()`, and if true, proceeds
4. `update_stats_enqueue_sleeper_dl(dl_rq, dl_se)` — called because `ENQUEUE_WAKEUP` flag is set
5. `__schedstats_from_dl_se(dl_se)` — attempts to get the task's `sched_statistics`
6. `dl_task_of(dl_se)` — triggers `BUG_ON` because `dl_server(dl_se)` is true

The same issue exists in `update_stats_wait_start_dl()` and `update_stats_wait_end_dl()`, which also call `__schedstats_from_dl_se()` and then `dl_task_of()`. These are called from `enqueue_task_dl()` (line 2151) and `pick_task_dl()`/`put_prev_task_dl()` (lines 2383, 2473) respectively. While these paths have callers that already guard against dl_server entities in some cases, the functions themselves are not safe to call with a dl_server `sched_dl_entity`.

The fundamental design oversight is that the schedstats infrastructure in `deadline.c` was written to assume every `sched_dl_entity` belongs to a real task. When deadline servers were introduced in v6.8, this invariant was broken but the schedstats functions were not updated to handle the new internal-only entities.

## Consequence

The immediate consequence is a kernel panic (`BUG_ON` → `Oops - BUG: Fatal exception`). This is a hard crash — the kernel cannot recover. The stack trace from the original report shows:

```
kernel BUG at kernel/sched/deadline.c:63!
Call trace:
  dl_task_of.part.0+0x0/0x10
  dl_server_start+0x54/0x158
  enqueue_task_fair+0x138/0x420
  enqueue_task+0x44/0xb0
  wake_up_new_task+0x1c0/0x3a0
  kernel_clone+0xe8/0x3e8
Kernel panic - not syncing: Oops - BUG: Fatal exception
SMP: stopping secondary CPUs
```

On multi-CPU systems, multiple CPUs can hit this simultaneously because every CPU has its own deadline server and every CFS task enqueue triggers the bug. The original report from an Ampere Altra system shows `SMP: failed to stop secondary CPUs 8-9,16,30,43,86,88,121,149`, indicating the panic cascaded across many cores.

The bug is 100% reproducible on any kernel v6.8+ compiled with `CONFIG_SCHEDSTATS=y` once schedstats is enabled at runtime (`echo 1 > /proc/sys/kernel/sched_schedstats`). Any CFS task creation, wakeup, or fork triggers the crash. The system becomes completely unusable — there is no way to avoid the crash other than not enabling schedstats.

## Fix Summary

The fix centralizes the dl_server and schedstat checks into `__schedstats_from_dl_se()`, making it return `NULL` for both dl_server entities and when schedstats is disabled:

```c
// Fixed version
static __always_inline struct sched_statistics *
__schedstats_from_dl_se(struct sched_dl_entity *dl_se)
{
    if (!schedstat_enabled())
        return NULL;

    if (dl_server(dl_se))
        return NULL;

    return &dl_task_of(dl_se)->stats;
}
```

The callers (`update_stats_wait_start_dl`, `update_stats_wait_end_dl`, `update_stats_enqueue_sleeper_dl`) are then simplified to check the return value:

```c
static inline void
update_stats_wait_start_dl(struct dl_rq *dl_rq, struct sched_dl_entity *dl_se)
{
    struct sched_statistics *stats = __schedstats_from_dl_se(dl_se);
    if (stats)
        __update_stats_wait_start(rq_of_dl_rq(dl_rq), dl_task_of(dl_se), stats);
}
```

This approach is cleaner than the original v3 patch (which added `if (dl_server(dl_se)) return;` to each caller individually). By moving both checks into `__schedstats_from_dl_se()`, the fix ensures that any future caller of this function is automatically protected against dl_server entities. The function annotation was also changed from `static inline` to `static __always_inline` to ensure the NULL checks are always inlined and can be optimized by the compiler when schedstats is a static key.

Note that `update_stats_dequeue_dl()` was NOT modified by this fix because its callers already guard against dl_server entities: the throttle path at line 1534 checks `if (!dl_server(dl_se))` before calling it, and `dequeue_task_dl()` only operates on real tasks.

## Triggering Conditions

The bug requires all of the following conditions:

1. **Kernel version v6.8 or later**: The deadline server infrastructure must be present (introduced by commit `63ba8422f876`).
2. **`CONFIG_SCHEDSTATS=y`**: The kernel must be compiled with scheduler statistics support. Without this, `schedstat_enabled()` is a compile-time constant `0` and the entire code path is eliminated.
3. **Runtime schedstats enabled**: Even with `CONFIG_SCHEDSTATS=y`, schedstats must be enabled at runtime. This is controlled by the static key `sched_schedstats`, toggled via `/proc/sys/kernel/sched_schedstats` (set to 1). Some distributions enable this by default for profiling.
4. **Any CFS task enqueue**: The bug is triggered by ANY CFS task being woken up, forked, or otherwise enqueued. This includes: `wake_up_new_task()` (fork/clone), `try_to_wake_up()` (wakeup from sleep), and `enqueue_task_fair()` from any path. The specific trigger in the original report was `kernel_clone()` → `wake_up_new_task()`.

There is NO race condition or timing requirement. The bug is completely deterministic: as soon as schedstats is enabled and any CFS task is enqueued, the crash occurs. On a multi-CPU system, the crash can happen simultaneously on multiple CPUs since each has its own deadline server.

The topology and CPU count are irrelevant — even a single-CPU system would trigger the bug. No cgroup configuration or special task priorities are needed. The default CFS scheduling class is sufficient.

## Reproduce Strategy (kSTEP)

This bug can be reproduced with kSTEP with one minor prerequisite: the kernel must be compiled with `CONFIG_SCHEDSTATS=y`, which is not currently in the default kSTEP kernel configuration.

### kSTEP Configuration Change

Add `CONFIG_SCHEDSTATS=y` to the kSTEP kernel configuration. This can be done by either:
- Adding `CONFIG_SCHEDSTATS=y` to `/users/szhong/kSTEP/linux/config.kstep`
- Creating an extra config file and passing it via `KSTEP_EXTRA_CONFIG`

### Driver Implementation Plan

1. **Task setup**: Create a single CFS task using `kstep_task_create()`. No special priority, affinity, or cgroup configuration is needed. The default CFS class is sufficient.

2. **Enable schedstats**: Before waking the task, enable schedstats at runtime:
   ```c
   kstep_sysctl_write("kernel/sched_schedstats", "%d", 1);
   ```

3. **Trigger the bug**: Wake up the CFS task using `kstep_task_wakeup()`. This will call `enqueue_task_fair()` → `dl_server_start()` → `enqueue_dl_entity()` → `update_stats_enqueue_dl()` → `update_stats_enqueue_sleeper_dl()` → `__schedstats_from_dl_se()` → `dl_task_of()` → `BUG_ON`.

4. **Detection**: On the buggy kernel, the `BUG_ON` will trigger a kernel panic. The QEMU guest will crash and the log will contain `kernel BUG at kernel/sched/deadline.c:63!`. The driver should use `kstep_pass()` / `kstep_fail()` based on whether the task was successfully woken and ran. On the buggy kernel, the system will crash before reaching the pass/fail check.

5. **Alternative detection**: Since BUG_ON causes a kernel panic, the driver may never reach its pass/fail logic on the buggy kernel. Detection can be done by checking the QEMU exit status or kernel log. On the fixed kernel, the task will wake up normally and the driver can log success.

### Expected Behavior

- **Buggy kernel** (v6.8 to v6.11): Kernel panic with `BUG at kernel/sched/deadline.c:63!` in `dl_task_of()`. The stack trace will show `dl_server_start` → `enqueue_dl_entity` → `update_stats_enqueue_dl`. The driver will not complete.

- **Fixed kernel** (v6.12-rc1+): The CFS task wakes up normally. The `__schedstats_from_dl_se()` function returns NULL for the dl_server entity, the stats update is skipped, and execution continues. The driver completes and reports pass.

### QEMU Configuration

- **CPUs**: 2 or more (CPU 0 reserved for the driver; the CFS task should run on CPU 1+). Even 2 CPUs is sufficient.
- **RAM**: Default is sufficient.
- **No special topology**: No SMT, NUMA, or cluster configuration needed.

### Kernel Build

The kernel must be built with `CONFIG_SCHEDSTATS=y`. This is the only non-default config option required. Example:

```bash
# Create extra config
echo "CONFIG_SCHEDSTATS=y" > /tmp/schedstats.config
# Build with extra config
make linux LINUX_NAME=dlserver_schedstats_buggy KSTEP_EXTRA_CONFIG=/tmp/schedstats.config
```

### Driver Pseudocode

```c
#include "driver.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,8,0)

static int driver_main(void)
{
    struct task_struct *t;

    /* Enable schedstats at runtime */
    kstep_sysctl_write("kernel/sched_schedstats", "%d", 1);

    /* Create a basic CFS task */
    t = kstep_task_create();
    if (!t) {
        kstep_fail("failed to create task");
        return -1;
    }

    /* Pin task to CPU 1 (away from driver CPU 0) */
    kstep_task_pin(t, 1, 1);

    /*
     * Wake the task. On buggy kernels, this triggers:
     * enqueue_task_fair() -> dl_server_start() -> enqueue_dl_entity()
     * -> update_stats_enqueue_dl() -> __schedstats_from_dl_se()
     * -> dl_task_of() -> BUG_ON(dl_server(dl_se))
     *
     * The kernel will panic before we reach kstep_pass().
     */
    kstep_task_wakeup(t);

    /* Allow ticks for the task to be scheduled */
    kstep_tick_repeat(10);

    /* If we reach here, the bug was NOT triggered (fixed kernel) */
    kstep_pass("CFS task enqueued without BUG_ON in dl_task_of");
    return 0;
}

#else
static int driver_main(void) { kstep_pass("N/A: pre-v6.8"); return 0; }
#endif
```

### Key Notes

- The bug requires absolutely minimal setup — just enabling schedstats and waking a CFS task. There is no race condition, no timing sensitivity, and no complex task interaction needed.
- The crash is deterministic and immediate. The very first CFS task enqueue after enabling schedstats will crash.
- The `BUG_ON` fires in the scheduler's enqueue hot path, so there is zero chance of the bug being intermittent — it will fire every single time on the buggy kernel.
