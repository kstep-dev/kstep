# Deadline: dl_cpu_busy() Panic Due to Empty Cgroup v2 Cpuset Mask

**Commit:** `b6e8d40d43ae4dec00c8fea2593eeea3114b8f44`
**Affected files:** `kernel/sched/core.c`, `kernel/cgroup/cpuset.c`, `include/linux/sched.h`
**Fixed in:** v6.0-rc1
**Buggy since:** v3.19-rc1 (commit `7f51412a415d` — "sched/deadline: Fix bandwidth check/update when migrating tasks between exclusive cpusets")

## Bug Description

When using cgroup v2, a cpuset's `cpus_allowed` mask can legitimately be empty. In cgroup v2 semantics, an empty `cpuset.cpus` means the cpuset inherits its effective CPU set from its parent. This is different from cgroup v1, where an empty `cpus_allowed` is never allowed for a usable cpuset.

The function `cpuset_can_attach()` in `kernel/cgroup/cpuset.c` is called during cgroup task migration — either when a task is moved to a new cgroup (via `cgroup.procs` write) or when cgroup CSS associations are updated (via `cgroup.subtree_control` write). This function calls `task_can_attach()` in `kernel/sched/core.c`, passing the cpuset's `cpus_allowed` mask as the second argument. When this mask is empty and the task being checked is a SCHED_DEADLINE task, the code attempts to find an active CPU within the (empty) cpuset using `cpumask_any_and(cpu_active_mask, cs_cpus_allowed)`. Since the intersection of any mask with an empty mask is empty, `cpumask_any_and()` returns `nr_cpu_ids` — a sentinel value indicating "no valid CPU found."

The returned `nr_cpu_ids` value is then passed directly to `dl_cpu_busy()`, which calls `dl_bw_of(cpu)`. This function performs a per-CPU data access using the CPU number as an index. Since `nr_cpu_ids` is out of bounds (beyond the range of valid per-CPU allocations), this results in an invalid memory access that triggers a kernel page fault — a hard crash.

The original commit `7f51412a415d` that introduced this code path was written for cgroup v1, where `cpus_allowed` is always non-empty for any usable cpuset. When cgroup v2 was later integrated, this assumption was silently broken, creating a latent crash that manifests whenever a SCHED_DEADLINE task's cgroup membership is evaluated against a cpuset with an empty `cpus_allowed`.

## Root Cause

The root cause is twofold: first, the caller `cpuset_can_attach()` passes `cs->cpus_allowed` instead of `cs->effective_cpus`; second, `task_can_attach()` fails to validate the result of `cpumask_any_and()` before using it.

In `kernel/cgroup/cpuset.c`, the function `cpuset_can_attach()` iterates over all tasks in the cgroup taskset and calls:

```c
ret = task_can_attach(task, cs->cpus_allowed);  /* BUGGY */
```

In cgroup v2, `cs->cpus_allowed` can be empty (meaning "inherit from parent"). The correct mask to use is `cs->effective_cpus`, which always contains the actual set of CPUs available to tasks in the cpuset — it is computed by propagating the parent's effective CPUs down the hierarchy and intersecting with any explicitly configured `cpuset.cpus`.

In `kernel/sched/core.c`, the function `task_can_attach()` contains this code path for SCHED_DEADLINE tasks:

```c
if (dl_task(p) && !cpumask_intersects(task_rq(p)->rd->span,
                                      cs_cpus_allowed)) {
    int cpu = cpumask_any_and(cpu_active_mask, cs_cpus_allowed);
    /* BUG: no check that cpu < nr_cpu_ids */
    ret = dl_cpu_busy(cpu, p);
}
```

When `cs_cpus_allowed` is empty:
1. `cpumask_intersects(task_rq(p)->rd->span, cs_cpus_allowed)` returns `false` because no CPU can intersect an empty mask. This causes the `if` branch to be taken.
2. `cpumask_any_and(cpu_active_mask, cs_cpus_allowed)` returns `nr_cpu_ids` because the AND of any mask with an empty mask is empty, and `cpumask_any_and()` returns `nr_cpu_ids` when no bit is set.
3. `dl_cpu_busy(nr_cpu_ids, p)` is called. Inside `dl_cpu_busy()`, the first operation is `dl_bw_of(cpu)` which expands to `&per_cpu(dl_bw, cpu)`. With `cpu = nr_cpu_ids`, this is an out-of-bounds per-CPU access that dereferences an unmapped address.

The logic error is that the code implicitly assumes `cs_cpus_allowed` (now `cs_effective_cpus`) is always non-empty when it reaches the SCHED_DEADLINE branch. This was true under cgroup v1 but not under cgroup v2.

## Consequence

The consequence is a hard kernel crash — specifically a page fault (BUG: unable to handle page fault) when accessing per-CPU data with an out-of-bounds CPU index. The crash occurs at `dl_cpu_busy+0x30/0x2b0` when it calls `dl_bw_of()` with `cpu = nr_cpu_ids`.

The full crash trace from the commit message:

```
[80468.182258] BUG: unable to handle page fault for address: ffffffff8b6648b0
  :
[80468.191019] RIP: 0010:dl_cpu_busy+0x30/0x2b0
  :
[80468.207946] Call Trace:
[80468.208947]  cpuset_can_attach+0xa0/0x140
[80468.209953]  cgroup_migrate_execute+0x8c/0x490
[80468.210931]  cgroup_update_dfl_csses+0x254/0x270
[80468.211898]  cgroup_subtree_control_write+0x322/0x400
```

This is a denial-of-service crash that can be triggered by any user with write access to cgroup v2 filesystem entries (typically root or a delegated cgroup owner). The crash takes down the entire kernel, requiring a hard reboot. Since cgroup v2 is the default cgroup mode on modern Linux distributions (systemd uses cgroup v2 by default), this bug is easily hit in production environments where SCHED_DEADLINE tasks are used alongside cgroup-based resource management.

## Fix Summary

The fix addresses both the caller and the callee to provide defense in depth.

First, in `kernel/cgroup/cpuset.c`, `cpuset_can_attach()` is changed to pass `cs->effective_cpus` instead of `cs->cpus_allowed`:

```c
ret = task_can_attach(task, cs->effective_cpus);  /* FIXED */
```

The `effective_cpus` field always contains the real set of CPUs available to the cpuset. For cgroup v1, `effective_cpus` is identical to `cpus_allowed`. For cgroup v2, `effective_cpus` is the computed result of propagating CPU masks down the hierarchy. This ensures the mask passed to `task_can_attach()` is never empty for a usable cpuset.

Second, in `kernel/sched/core.c`, `task_can_attach()` adds a bounds check on the result of `cpumask_any_and()`:

```c
int cpu = cpumask_any_and(cpu_active_mask, cs_effective_cpus);
if (unlikely(cpu >= nr_cpu_ids))
    return -EINVAL;
ret = dl_cpu_busy(cpu, p);
```

This guard prevents the out-of-bounds per-CPU access even if, through some unforeseen code path, the effective cpuset mask ends up empty. The function parameter name is also renamed from `cs_cpus_allowed` to `cs_effective_cpus` to document the semantic change.

## Triggering Conditions

The bug requires the following specific conditions:

1. **Cgroup v2 mode**: The system must be using cgroup v2 (unified hierarchy). In cgroup v1, an empty `cpus_allowed` would be rejected earlier in `cpuset_can_attach()` by the `cpumask_empty(cs->cpus_allowed)` check that returns `-ENOSPC`. In v2 mode, this check is skipped (gated by `!is_in_v2_mode()`).

2. **SCHED_DEADLINE task**: There must be at least one task with `SCHED_DEADLINE` scheduling policy. The buggy code path in `task_can_attach()` is gated by `dl_task(p)`. CFS and RT tasks do not enter this branch.

3. **Empty cpuset.cpus in destination cpuset**: The target cgroup's cpuset must have an empty `cpus_allowed` mask. In cgroup v2, this happens when a child cpuset is created but `cpuset.cpus` is never explicitly written — the default is empty, meaning "inherit effective CPUs from parent."

4. **Task migration trigger**: The `cpuset_can_attach()` function must be invoked. This happens through two paths:
   - Writing a SCHED_DEADLINE task's PID to `cgroup.procs` of the target cgroup (direct task migration).
   - Writing to `cgroup.subtree_control` of an ancestor, which triggers `cgroup_update_dfl_csses()` → `cgroup_migrate_execute()`, implicitly migrating all tasks in affected cgroups.

5. **Root domain mismatch**: The DL task's current root domain span must not intersect with the (empty) cpuset mask. Since the mask is empty, this condition is always true — no CPU can intersect an empty mask. So this condition is automatically satisfied when the cpuset mask is empty.

The bug is 100% deterministic when these conditions are met — there is no race condition or timing dependency. Any system running cgroup v2 with SCHED_DEADLINE tasks that encounters the above cgroup operations will crash.

## Reproduce Strategy (kSTEP)

To reproduce this bug in kSTEP, the following approach should be used. The core idea is to create a SCHED_DEADLINE task, create a cgroup v2 cpuset child without setting `cpuset.cpus` (leaving `cpus_allowed` empty), and then move the DL task into that cpuset to trigger `cpuset_can_attach()` → `task_can_attach()` with an empty mask.

### Required kSTEP Extension: SCHED_DEADLINE Task Support

kSTEP currently lacks an API to create SCHED_DEADLINE tasks. A new helper function is needed:

```c
void kstep_task_deadline(struct task_struct *p, u64 runtime_ns, u64 deadline_ns, u64 period_ns);
```

This function should call the kernel's internal `__sched_setscheduler()` or `sched_setattr()` to set the task's scheduling policy to `SCHED_DEADLINE` with the specified runtime, deadline, and period parameters. This is analogous to the existing `kstep_task_fifo(p)` function which sets `SCHED_FIFO` policy. A reasonable set of parameters would be: runtime=5ms, deadline=10ms, period=10ms.

### Step-by-Step Driver Plan

1. **Configure QEMU with at least 2 CPUs** (CPU 0 is reserved for the driver, CPU 1+ for the test task).

2. **Create a kthread** using `kstep_kthread_create("dl_test")` and bind it to CPU 1 using `kstep_kthread_bind(p, cpumask_of(1))`. Start it with `kstep_kthread_start(p)`.

3. **Set the task to SCHED_DEADLINE** using the new `kstep_task_deadline(p, 5000000, 10000000, 10000000)` function (5ms runtime, 10ms deadline/period).

4. **Create a child cgroup** using `kstep_cgroup_create("test_cpuset")`. This creates a cgroup v2 child and enables `+cpu +cpuset` subtree_control. Critically, do NOT call `kstep_cgroup_set_cpuset("test_cpuset", ...)` — this leaves `cpuset.cpus` (and thus `cpus_allowed`) empty.

5. **Attempt to move the DL task into the empty cpuset** by calling `kstep_cgroup_add_task("test_cpuset", p->pid)`. This writes `p->pid` to `cgroup.procs`, which triggers:
   - `cgroup_procs_write()` → `cgroup_attach_task()` → `cgroup_migrate()` → `cgroup_migrate_execute()` → `cpuset_can_attach()` → `task_can_attach(task, cs->cpus_allowed)` [buggy] or `task_can_attach(task, cs->effective_cpus)` [fixed].

6. **On the buggy kernel**: The empty `cpus_allowed` mask is passed to `task_can_attach()`. Since `dl_task(p)` is true and `cpumask_intersects()` with empty mask returns false, the code calls `cpumask_any_and()` which returns `nr_cpu_ids`, then `dl_cpu_busy(nr_cpu_ids, p)` causes a page fault crash. The kernel will panic or oops.

7. **On the fixed kernel**: The `effective_cpus` mask (which inherits from the parent and contains all CPUs) is passed instead. Since `cpumask_intersects(task_rq(p)->rd->span, cs_effective_cpus)` returns true (the root domain spans the effective CPUs), the `dl_task()` branch is NOT taken, and `task_can_attach()` returns 0 (success) or handles the case gracefully with the `cpu >= nr_cpu_ids` guard.

### Detection Logic

- **Pass condition**: On the fixed kernel, `kstep_cgroup_add_task()` should either succeed (return without error) or fail gracefully with `-EINVAL` (from the new bounds check). The kernel should remain stable. Call `kstep_pass("DL task cgroup migration handled gracefully")`.

- **Fail condition**: On the buggy kernel, the kernel will crash with a page fault at `dl_cpu_busy()`. This will manifest as a kernel oops/panic. The driver may not get a chance to call `kstep_fail()` since the crash kills the system, but the absence of a pass message in the log (or the presence of a kernel oops in `dmesg`) serves as the failure indicator.

### Alternative Detection Approach

Since a kernel crash prevents explicit logging, the driver should set up a marker before the dangerous operation:

```c
kstep_pass("about to trigger cpuset attach");
kstep_cgroup_add_task("test_cpuset", p->pid);
kstep_pass("cpuset attach completed without crash");
```

On the buggy kernel, only the first message will appear in the log. On the fixed kernel, both messages will appear. The test harness should check for the presence of the second message.

### Cgroup v2 Considerations

kSTEP's `kstep_cgroup_init()` already sets up cgroup v2 with `+cpu +cpuset` subtree_control at the root. The `kstep_cgroup_create()` function creates child cgroups under `/sys/fs/cgroup/` and enables the same controllers. For this bug, we specifically need a cpuset with empty `cpus_allowed`, which is the default state when `cpuset.cpus` is never written in cgroup v2.

One potential complication: the kernel may reject the cgroup_procs write for other reasons (e.g., the cpuset might not be a valid migration target). If so, an alternative trigger is to first place the DL task in the cgroup, then manipulate `subtree_control` of an ancestor to trigger `cgroup_update_dfl_csses()`, which calls `cpuset_can_attach()` for all tasks in affected cgroups.

### Summary of kSTEP Changes Needed

- Add `kstep_task_deadline(p, runtime, deadline, period)` function to `kmod/kernel.c` and `kmod/driver.h` — this is a small wrapper around the kernel's `sched_setattr()` or `__sched_setscheduler()` internal function, analogous to the existing `kstep_task_fifo()`.

No other framework changes are required. The existing cgroup v2 support (`kstep_cgroup_create`, `kstep_cgroup_add_task`, `kstep_cgroup_write`) is sufficient for the cgroup operations.
