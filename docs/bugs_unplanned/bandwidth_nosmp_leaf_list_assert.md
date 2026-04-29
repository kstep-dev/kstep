# Bandwidth: cfs_rq_is_decayed() always true on !SMP breaks leaf_cfs_rq_list

**Commit:** `c0490bc9bb62d9376f3dd4ec28e03ca0fef97152`
**Affected files:** kernel/sched/fair.c
**Fixed in:** v6.7-rc1
**Buggy since:** v5.19-rc1 (introduced by commit `0a00a354644e` "sched/fair: Delete useless condition in tg_unthrottle_up()")

## Bug Description

On uniprocessor (!SMP) kernels with CFS bandwidth throttling enabled (CONFIG_CFS_BANDWIDTH=y, CONFIG_FAIR_GROUP_SCHED=y), the `cfs_rq_is_decayed()` function unconditionally returns `true`. This causes the `leaf_cfs_rq_list` — a per-runqueue linked list that tracks which CFS runqueues have active load — to not be properly maintained during CFS bandwidth unthrottle operations. Specifically, when a throttled cfs_rq is unthrottled via `tg_unthrottle_up()`, the condition `!cfs_rq_is_decayed(cfs_rq)` is always `false`, so `list_add_leaf_cfs_rq()` is never called, even when the cfs_rq has running tasks.

The `leaf_cfs_rq_list` on SMP kernels is primarily used for load tracking propagation and load balancing. On !SMP kernels, these functionalities are unnecessary, so the maintenance of this list was historically considered unimportant. However, the sched debug interface (`/proc/sched_debug`) iterates over `leaf_cfs_rq_list` to print per-cfs_rq statistics, meaning the list must still be correct even on !SMP.

Before the buggy commit `0a00a354644e`, the function `tg_unthrottle_up()` contained the condition `if (!cfs_rq_is_decayed(cfs_rq) || cfs_rq->nr_running)`, which had an explicit fallback check on `cfs_rq->nr_running`. Even though `cfs_rq_is_decayed()` always returned `true` on !SMP (making the first clause always `false`), the second clause `cfs_rq->nr_running` correctly added cfs_rqs with running tasks to the leaf list. When commit `0a00a354644e` removed the `|| cfs_rq->nr_running` fallback — reasoning that on SMP, `cfs_rq_is_decayed()` already checks `cfs_rq->load.weight` which subsumes `nr_running` — it broke the !SMP case where `cfs_rq_is_decayed()` was a trivial stub.

The result is that after unthrottling, cfs_rqs with running entities are not added to the `leaf_cfs_rq_list`. This leaves `rq->tmp_alone_branch` in an inconsistent state, and the `assert_list_leaf_cfs_rq()` check (which verifies `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list`) fires, producing a `WARN_ON_ONCE` warning and tainting the kernel.

## Root Cause

The root cause lies in the interplay between two code regions in `kernel/sched/fair.c`:

**The !SMP stub of `cfs_rq_is_decayed()`:** On SMP kernels, `cfs_rq_is_decayed()` (around line 4129) performs a comprehensive check:
```c
static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
    if (cfs_rq->load.weight)
        return false;
    if (!load_avg_is_decayed(&cfs_rq->avg))
        return false;
    if (child_cfs_rq_on_list(cfs_rq))
        return false;
    if (cfs_rq->tg_load_avg_contrib)
        return false;
    return true;
}
```
On !SMP kernels (inside the `#else /* CONFIG_SMP */` block around line 4867), the function was a trivial stub:
```c
static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
    return true;
}
```
This stub unconditionally reports that every cfs_rq is "decayed" (has no load), regardless of whether tasks are actually enqueued on it.

**The `tg_unthrottle_up()` function:** When a CFS bandwidth period timer fires and distributes new runtime to a throttled cfs_rq, the `unthrottle_cfs_rq()` function calls `walk_tg_tree_from(cfs_rq->tg, tg_nop, tg_unthrottle_up, (void *)rq)` to walk up the task group hierarchy and unthrottle each level. Inside `tg_unthrottle_up()`, after decrementing the throttle count, the code adds the cfs_rq to the leaf list:
```c
if (!cfs_rq_is_decayed(cfs_rq))
    list_add_leaf_cfs_rq(cfs_rq);
```
On !SMP, `cfs_rq_is_decayed()` always returns `true`, so this `if` condition is always `false`, and `list_add_leaf_cfs_rq()` is never called. This means cfs_rqs that have running tasks are never added back to the leaf list after being removed during throttling (by `tg_throttle_down()` → `list_del_leaf_cfs_rq()`).

**The assertion failure:** Later in `unthrottle_cfs_rq()`, after the tree walk completes and entities are re-enqueued, `assert_list_leaf_cfs_rq(rq)` is called (around line 6092). This assertion checks `rq->tmp_alone_branch != &rq->leaf_cfs_rq_list` — i.e., it verifies that all branches of the leaf list tree have been properly connected. Because `tg_unthrottle_up()` failed to add the cfs_rq to the leaf list, `tmp_alone_branch` still points to a stale location, and the assertion fires.

The original commit `0a00a354644e` removed the `|| cfs_rq->nr_running` guard in `tg_unthrottle_up()` based on the reasoning that on SMP, `cfs_rq_is_decayed()` checks `cfs_rq->load.weight`, which is non-zero whenever `nr_running > 0`. This reasoning is correct for the SMP version but completely wrong for the !SMP stub, which was never updated to reflect actual cfs_rq state.

## Consequence

The immediate consequence is a `WARN_ON_ONCE` warning triggered by `assert_list_leaf_cfs_rq()` in `unthrottle_cfs_rq()`. The warning message is:
```
rq->tmp_alone_branch != &rq->leaf_cfs_rq_list
WARNING: CPU: 0 PID: 0 at kernel/sched/fair.c:437 unthrottle_cfs_rq+0x3b4/0x3b8
```
The stack trace shows the call chain: `riscv_timer_interrupt` → `hrtimer_interrupt` → `__hrtimer_run_queues` → `sched_cfs_period_timer` → `distribute_cfs_runtime` → `unthrottle_cfs_rq`. This means the warning fires in hard interrupt context (the hrtimer for the CFS bandwidth period timer), during PID 0 (the idle/swapper task).

The warning taints the kernel, causing test frameworks like LTP (Linux Test Project) to report failures. The specific test that triggers this is `cfs_bandwidth01`, which creates cgroups with CPU bandwidth limits and runs worker tasks. The LTP test infrastructure checks for kernel taint and reports `TFAIL` when detected.

Beyond the warning, the corrupted `leaf_cfs_rq_list` means that `for_each_leaf_cfs_rq_safe()` iterations — used by sched debug output and potentially other paths that walk leaf cfs_rqs — may miss cfs_rqs with active tasks. On !SMP this has limited practical scheduling impact since there is no load balancing, but it corrupts the sched debug output and violates internal data structure invariants, which could mask or compound other bugs.

## Fix Summary

The fix changes the !SMP stub of `cfs_rq_is_decayed()` from unconditionally returning `true` to checking whether the cfs_rq has any running entities:

```c
// Before (buggy):
static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
    return true;
}

// After (fixed):
static inline bool cfs_rq_is_decayed(struct cfs_rq *cfs_rq)
{
    return !cfs_rq->nr_running;
}
```

This is the minimal correct fix for !SMP. On uniprocessor kernels, there is no PELT load tracking (`load_avg`, `util_avg`), no `tg_load_avg_contrib`, and no child cfs_rq list maintenance for load propagation. The only meaningful indicator of whether a cfs_rq is "active" is whether it has running entities (`nr_running > 0`). When `nr_running` is non-zero, the cfs_rq has tasks and should be on the leaf list; when zero, it is truly decayed and can be removed.

The initial patch (v1) proposed checking `cfs_rq->load.weight` instead, which also works since `load.weight` is non-zero when tasks are enqueued. However, reviewer Vincent Guittot suggested using `!cfs_rq->nr_running` as being clearer and more directly expressing the intent. The v2 patch (and final committed version) adopted this suggestion.

This fix is both correct and complete: it ensures `tg_unthrottle_up()` calls `list_add_leaf_cfs_rq()` for cfs_rqs that have running tasks on !SMP, maintaining the `leaf_cfs_rq_list` invariant and preventing the `assert_list_leaf_cfs_rq()` warning.

## Triggering Conditions

The following precise conditions are needed to trigger this bug:

1. **Kernel configuration:** The kernel MUST be compiled with `CONFIG_SMP` disabled (uniprocessor). Additionally, `CONFIG_FAIR_GROUP_SCHED=y` and `CONFIG_CFS_BANDWIDTH=y` must be enabled to get CFS bandwidth throttling with task group hierarchies and the `leaf_cfs_rq_list` machinery.

2. **CPU count:** Exactly 1 CPU (this is implied by !SMP). The reporter used QEMU with a RISC-V 64-bit virtual machine (`qemu_riscv64_virt_defconfig` from Buildroot).

3. **Cgroup setup:** At least one non-root cgroup with CPU bandwidth limits configured. The cgroup must have a `cpu.max` (v2) or `cpu.cfs_quota_us`/`cpu.cfs_period_us` (v1) setting that restricts CPU time. The reporter's LTP test created three worker cgroups with bandwidth limits like `3000 10000` (30% of CPU), `2000 10000` (20%), and `3000 10000` (30%).

4. **Workload:** Tasks running within the bandwidth-limited cgroups that consume enough CPU time to actually get throttled. The tasks must exhaust their cgroup's quota within a period, causing the `sched_cfs_period_timer` to fire, distribute new runtime, and unthrottle the cfs_rq. This is straightforward with CPU-bound tasks.

5. **Trigger sequence:** The bug triggers during the unthrottle path: `sched_cfs_period_timer` → `distribute_cfs_runtime` → `unthrottle_cfs_rq` → `walk_tg_tree_from(..., tg_unthrottle_up, ...)` → `cfs_rq_is_decayed()` returns `true` → `list_add_leaf_cfs_rq()` not called → `assert_list_leaf_cfs_rq()` fires. This happens automatically when the period timer distributes runtime to a previously throttled cfs_rq that has queued tasks.

6. **Reliability:** The bug is 100% deterministic and reliable once the above conditions are met. It triggers on every unthrottle operation where the cfs_rq has running tasks. The reporter reproduced it easily with the LTP `cfs_bandwidth01` test.

7. **Kernel version:** The bug exists from v5.19-rc1 (when commit `0a00a354644e` removed the `|| cfs_rq->nr_running` guard) through v6.6 (the last release before the fix landed in v6.7-rc1).

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP. Below are the detailed reasons and analysis:

### 1. Why this bug cannot be reproduced with kSTEP

The bug exists exclusively in the `#else /* CONFIG_SMP */` code path of `cfs_rq_is_decayed()` in `kernel/sched/fair.c`. This code is only compiled into uniprocessor kernels — i.e., kernels built with `CONFIG_SMP` disabled. kSTEP's kernel configuration (`linux/config.kstep`) explicitly sets `CONFIG_SMP=y`, and this is a fundamental, non-negotiable part of kSTEP's design. The SMP version of `cfs_rq_is_decayed()` (which checks `cfs_rq->load.weight`, `load_avg_is_decayed()`, `child_cfs_rq_on_list()`, and `tg_load_avg_contrib`) is correct and has always been correct — the bug never existed in this code path.

When kSTEP builds a kernel, the buggy `return true;` stub is never compiled into the binary. The preprocessor `#else /* CONFIG_SMP */` branch is dead code. Therefore, no matter what driver we write — no matter what tasks, cgroups, bandwidth limits, or unthrottle sequences we create — we cannot trigger the bug because the buggy code simply does not exist in the compiled kernel.

### 2. What would need to change in kSTEP to support this

To reproduce this bug, kSTEP would need to build a kernel with `CONFIG_SMP=n`. This is not a minor change — it is a fundamental architectural shift:

- **Topology APIs become meaningless:** All of kSTEP's topology setup (`kstep_topo_init()`, `kstep_topo_set_smt/cls/mc/pkg/node()`, `kstep_topo_apply()`) is predicated on SMP. Without SMP, there is only one CPU and no scheduling domains.
- **Load balancing is disabled:** The entire load balancing subsystem (`load_balance()`, `find_busiest_group/queue()`, `newidle_balance()`) does not exist on !SMP kernels. Many kSTEP callbacks (`on_sched_balance_begin`, `on_sched_balance_selected`) become meaningless.
- **PELT load tracking is disabled:** All PELT-related functionality (`update_load_avg()`, `load_avg`, `util_avg`, `util_est`) is stubbed out on !SMP. `kstep_cpu_set_freq()` and `kstep_cpu_set_capacity()` have no effect.
- **CPU pinning is trivial:** `kstep_task_pin(p, begin, end)` can only pin to CPU 0. CPU 0 reservation for the driver is impossible since it's the only CPU.
- **QEMU must run with 1 vCPU:** kSTEP would need to configure QEMU with `-smp 1`.

In essence, disabling SMP would reduce kSTEP to a single-CPU scheduling testbed, breaking the majority of its existing drivers and APIs. This is not a minor extension but a different operating mode entirely.

### 3. Classification

This is fundamentally a **CONFIG_SMP=n only bug** that cannot be triggered on any SMP kernel, regardless of how many CPUs are configured or what workload is run. kSTEP is an SMP-focused framework, and the buggy code path is compiled out of its kernels.

### 4. Alternative reproduction methods

The bug can be reliably reproduced outside kSTEP using the method described in the original bug report:

1. Build a RISC-V 64-bit (or any architecture) kernel with `CONFIG_SMP=n`, `CONFIG_FAIR_GROUP_SCHED=y`, and `CONFIG_CFS_BANDWIDTH=y`.
2. Boot the kernel in QEMU with a single vCPU.
3. Run the LTP `cfs_bandwidth01` test, which creates cgroups with CPU bandwidth limits and runs worker tasks.
4. Observe the `WARN_ON_ONCE` in `assert_list_leaf_cfs_rq()` in dmesg.

Alternatively, any simple userspace program that creates a cgroup with a CPU bandwidth limit (e.g., `echo "3000 10000" > /sys/fs/cgroup/cpu/test/cpu.max`) and then runs a CPU-intensive workload in that cgroup will trigger the bug once the bandwidth quota is exhausted and the period timer fires to unthrottle the cfs_rq.
