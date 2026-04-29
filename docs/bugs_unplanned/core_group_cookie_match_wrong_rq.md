# Core: sched_group_cookie_match() Always Checks Caller's RQ Instead of Iterated CPU's RQ

**Commit:** `e705968dd687574b6ca3ebe772683d5642759132`
**Affected files:** kernel/sched/sched.h
**Fixed in:** v6.1-rc2
**Buggy since:** v5.14-rc1 (introduced by commit `97886d9dcd86` "sched: Migration changes for core scheduling")

## Bug Description

The Linux kernel's core scheduling feature (`CONFIG_SCHED_CORE`) allows grouping tasks into trust domains via cookies. When core scheduling is active, sibling SMT threads on the same physical core are constrained to run tasks with matching cookies or forced idle, preventing side-channel attacks between mutually untrusted workloads. To make this efficient, the scheduler's task placement paths (wakeup slow path, load balancing) consult cookie-matching functions to avoid placing a task on a core where its cookie would conflict with already-running tasks.

The function `sched_group_cookie_match()` was introduced in commit `97886d9dcd86` ("sched: Migration changes for core scheduling") to determine whether any CPU in a given scheduling group has a core cookie compatible with a waking task. It is called from `find_idlest_group()` in the task wakeup slow path (`select_task_rq_fair()` → `find_idlest_cpu()` → `find_idlest_group()`). The intent is to skip scheduling groups where no CPU's core cookie matches the task, avoiding forced idle situations on the destination core.

However, the implementation of `sched_group_cookie_match()` contained a critical copy-paste bug: the `for_each_cpu_and()` loop iterates over each CPU in the scheduling group intersected with the task's `cpus_ptr`, but inside the loop body, it calls `sched_core_cookie_match(rq, p)` using the caller's `rq` parameter rather than `cpu_rq(cpu)` for the currently iterated CPU. This means every iteration checks the same runqueue — the one belonging to the CPU that initiated the wakeup — rather than the runqueue of each candidate CPU in the group.

## Root Cause

The root cause is a simple variable reference error in the loop body of `sched_group_cookie_match()`. The function signature is:

```c
static inline bool sched_group_cookie_match(struct rq *rq,
                                            struct task_struct *p,
                                            struct sched_group *group)
```

The parameter `rq` is the runqueue of the CPU calling `find_idlest_group()` (passed as `cpu_rq(this_cpu)`). The function is supposed to iterate over all CPUs in `group` that the task is allowed to run on, and check whether any of those CPUs has a core cookie that matches the task. The buggy implementation was:

```c
for_each_cpu_and(cpu, sched_group_span(group), p->cpus_ptr) {
    if (sched_core_cookie_match(rq, p))   /* BUG: always checks caller's rq */
        return true;
}
return false;
```

The variable `cpu` iterates through different CPUs in the group, but it is never used — the function always passes the same `rq` (the caller's runqueue) to `sched_core_cookie_match()`. The correct code should be `sched_core_cookie_match(cpu_rq(cpu), p)`.

The `sched_core_cookie_match()` function checks whether the core containing the given rq is either fully idle (in which case any cookie is acceptable) or has a `core_cookie` matching `p->core_cookie`. Because the buggy code always passes the same `rq`, the result of `sched_core_cookie_match(rq, p)` is invariant across all loop iterations. This means:

1. If the caller's own core cookie matches the task (or the caller's core is idle), `sched_group_cookie_match()` returns `true` for every group that has at least one CPU in the task's `cpus_ptr` — even groups where no CPU actually has a compatible cookie.
2. If the caller's core cookie does not match the task and the caller's core is not idle, `sched_group_cookie_match()` returns `false` for every group — even groups that contain CPUs with perfectly matching cookies.

Additionally, because the `cpu_rq()` macro was defined later in `sched.h` (after the `sched_group_cookie_match()` function), the fix also had to move the `cpu_rq()` and related macro definitions earlier in the header file so they could be used inside `sched_group_cookie_match()`.

## Consequence

The consequence is incorrect scheduling group selection during the task wakeup slow path when core scheduling is enabled. This manifests in two ways:

**Case 1: Caller's cookie matches** — All scheduling groups pass the cookie check regardless of their actual cookie state. This defeats the purpose of cookie-aware group selection: a task may be placed in a group where its cookie conflicts with running tasks on the destination core, causing forced idle on sibling SMT threads. This increases forced idle time, degrading throughput for core-scheduling workloads. The scheduler believes the group is compatible, but upon actual scheduling, the task forces its sibling thread idle.

**Case 2: Caller's cookie doesn't match** — All scheduling groups fail the cookie check, even those with compatible cookies. The `find_idlest_group()` function skips all non-local groups (since the cookie check fails for every group), potentially returning `NULL` (no idlest group found). This forces the task to stay on the local group even when remote groups have idle cores with matching cookies. The result is suboptimal task placement, poor load distribution, and increased forced idle time on the local core if the local core's cookie also doesn't match.

In practice, this bug causes performance degradation for workloads that rely on core scheduling for security isolation (e.g., cloud/container workloads isolating tenants). The scheduler makes suboptimal placement decisions, increasing forced idle time and reducing overall CPU utilization. There is no crash or data corruption — the impact is purely on scheduling quality and throughput.

## Fix Summary

The fix is a one-line change in the loop body of `sched_group_cookie_match()`:

```c
-       if (sched_core_cookie_match(rq, p))
+       if (sched_core_cookie_match(cpu_rq(cpu), p))
```

This ensures that each iteration of the `for_each_cpu_and()` loop checks the runqueue of the CPU currently being iterated (`cpu`), rather than always checking the caller's runqueue. Now the function correctly determines whether any CPU in the scheduling group has a core whose cookie state is compatible with the task being placed.

To enable the use of `cpu_rq()` inside `sched_group_cookie_match()`, the fix also moves the `DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues)` declaration and the `cpu_rq()`, `this_rq()`, `task_rq()`, `cpu_curr()`, and `raw_rq()` macro definitions earlier in `sched.h` — from after the `CONFIG_SCHED_SMT` block to just after the `is_migration_disabled()` function, before the `CONFIG_SCHED_CORE` block. This is a purely mechanical move with no semantic change.

The fix is correct and complete: after the change, `sched_group_cookie_match()` properly evaluates cookie compatibility for each CPU in the scheduling group, allowing `find_idlest_group()` to make accurate decisions about which groups to consider for task placement.

## Triggering Conditions

The bug requires the following conditions to be triggered:

1. **CONFIG_SCHED_CORE must be enabled** in the kernel configuration. Core scheduling is enabled at build time with this config option.

2. **Core scheduling must be active at runtime.** Tasks must have been assigned core scheduling cookies via `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, ...)` or `prctl(PR_SCHED_CORE, PR_SCHED_CORE_SHARE_TO, ...)`. This sets the `__sched_core_enabled` static key and assigns non-zero `core_cookie` values to tasks.

3. **SMT topology is required.** The system must have at least two logical CPUs per physical core (SMT/Hyper-Threading). Core scheduling's cookie mechanism only has effect on SMT systems where sibling threads share a physical core.

4. **Multiple scheduling groups with different cookie states.** The system must have a scheduling domain hierarchy where `find_idlest_group()` traverses multiple scheduling groups. At least two groups must exist where different CPUs have different core cookies, so the bug's incorrect comparison produces wrong results.

5. **Task wakeup must go through the slow path.** The task being woken up must not find an idle CPU in the fast path (`select_idle_sibling()`), forcing the scheduler to fall through to `find_idlest_cpu()` → `find_idlest_group()`. This typically happens when the system is moderately loaded and no idle CPU is immediately available in the task's preferred domain.

6. **The waking CPU's core cookie must differ from at least one group's cookies.** If the waking CPU's core is idle or has a universally matching cookie, the bug is masked (case 1 above — all groups pass). The bug is most visible when the waking CPU's core has an active, non-matching cookie, causing all groups to be incorrectly rejected.

The bug is deterministic once the above conditions are met — it is not a race condition. Every invocation of `sched_group_cookie_match()` will produce the wrong result for the described scenarios.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP in its current form for the following reasons:

### 1. Core Scheduling Requires Userspace prctl() Syscall

Core scheduling is activated by assigning cookies to tasks via the `prctl(PR_SCHED_CORE, ...)` system call. This is a userspace-initiated operation that goes through `sched_core_share_pid()` in `kernel/sched/core.c`, which:
- Allocates a core scheduling cookie via `sched_core_alloc_task_cookie()`
- Sets `task->core_cookie` to a non-zero value
- Enables the `__sched_core_enabled` static key (globally activating core scheduling)
- Calls `sched_core_update_cookie()` to update the core's state

kSTEP cannot intercept userspace syscalls directly. Tasks in kSTEP are kernel-controlled and cannot issue `prctl()` calls. There is no kSTEP API to enable core scheduling or assign cookies to tasks.

### 2. Potential Internal State Manipulation Is Fragile

While kSTEP provides `KSYM_IMPORT()` to access internal kernel symbols and can read/write `rq` fields via `cpu_rq()`, manually enabling core scheduling would require:
- Importing and calling `static_branch_enable(&__sched_core_enabled)` to activate the static key
- Setting `rq->core_enabled = 1` for each relevant CPU
- Setting `rq->core = rq` (or the appropriate core leader rq) for each SMT sibling
- Setting `rq->core->core_cookie` to appropriate values
- Setting `task->core_cookie` for each task
- Properly initializing the core scheduling rb-tree (`rq->core_tree`)
- Calling `queue_core_balance()` and other initialization paths

This level of internal state manipulation is extremely fragile — missing any initialization step could cause kernel crashes. The core scheduling subsystem has complex invariants around locking (`rq_lockp()` behavior changes when core scheduling is active), the core-wide task selection algorithm (`pick_next_task()` changes), and the forced-idle mechanism. Manually enabling these without going through the proper initialization path risks undefined behavior.

### 3. The Wakeup Slow Path Requires Specific Load Conditions

Even if core scheduling could be enabled, triggering `find_idlest_group()` requires the fast path (`select_idle_sibling()`) to fail to find a suitable CPU. This means the system must be moderately loaded with no immediately available idle CPU in the task's LLC domain. While kSTEP can create multiple tasks to load the system, orchestrating the exact load distribution across scheduling domains to force the slow path is non-trivial.

### 4. What Would Need to Be Added to kSTEP

To support reproducing core scheduling bugs, kSTEP would need fundamental additions:
- **`kstep_core_sched_create_cookie()`**: Allocate a new core scheduling cookie
- **`kstep_task_set_cookie(p, cookie)`**: Assign a cookie to a task, properly going through `sched_core_update_cookie()` to maintain internal consistency
- **`kstep_core_sched_enable()`**: Safely enable the `__sched_core_enabled` static key and initialize per-rq core scheduling state
- **SMT-aware topology initialization**: Ensure `cpu_smt_mask()` returns correct sibling masks and `rq->core` pointers are properly set up across SMT siblings

These additions are **not minor** — they require carefully replicating the initialization sequence from `prctl(PR_SCHED_CORE, ...)` → `sched_core_share_pid()` → `sched_core_update_cookie()`, which involves cookie allocation, reference counting, core-wide lock initialization, and static key management. Getting any of these wrong could destabilize the entire scheduler.

### 5. Alternative Reproduction Methods

Outside kSTEP, this bug can be reproduced on a real or virtual SMT system running a kernel between v5.14 and v6.1-rc1 with `CONFIG_SCHED_CORE=y`:

1. Boot a system with SMT enabled (e.g., 2 cores × 2 threads = 4 CPUs)
2. Create two groups of tasks with different core scheduling cookies using `prctl(PR_SCHED_CORE, PR_SCHED_CORE_CREATE, ...)`
3. Pin cookie-group-A tasks to core 0 (CPUs 0,1) and cookie-group-B tasks to core 1 (CPUs 2,3)
4. Wake a task from group-A on a CPU in core 1, forcing it through `find_idlest_group()`
5. Observe (via ftrace or scheduler tracepoints) that `sched_group_cookie_match()` returns incorrect results — it checks the waking CPU's core cookie for all groups instead of each group's CPUs
6. Compare task placement decisions between buggy and fixed kernels using `sched:sched_wakeup` and `sched:sched_migrate_task` tracepoints
