# Core: Spurious WARN_ON_ONCE in sched_setaffinity on cpuset race

**Commit:** `70ee7947a29029736a1a06c73a48ff37674a851b`
**Affected files:** `kernel/sched/syscalls.c`
**Fixed in:** v6.13-rc3
**Buggy since:** v6.2-rc1 (introduced by commit `8f9ea86fdf99b` "sched: Always preserve the user requested cpumask")

## Bug Description

The `__sched_setaffinity()` function in `kernel/sched/syscalls.c` contains a `WARN_ON_ONCE()` that fires spuriously when a per-task CPU affinity assignment via `sched_setaffinity()` races with a concurrent cpuset cgroup update. The warning was introduced by commit `8f9ea86fdf99b` which added logic to preserve the user-requested cpumask across cpuset changes. That commit included a `WARN_ON_ONCE(empty)` guard intended to flag an "impossible" situation where the cpuset-allowed mask has zero overlap with the previously stored user affinity mask.

However, this situation is not impossible at all — it is trivially constructible. When one thread repeatedly changes a cpuset's `cpuset.cpus` to shrink and expand the set of allowed CPUs, and another thread repeatedly calls `sched_setaffinity()` to pin a task to a CPU that is only intermittently in the cpuset, the two operations can race. Specifically, the cpuset can be narrowed between the first `__set_cpus_allowed_ptr()` call and the subsequent `cpuset_cpus_allowed()` check inside `__sched_setaffinity()`. When this happens, the new cpuset-allowed mask may have zero intersection with the user's requested affinity, triggering the `WARN_ON_ONCE`.

The code path that triggers the warning is a legitimate fallback: when the task's requested affinity no longer fits within the cpuset, the kernel falls back to using the cpuset's allowed mask directly. The additional check for overlap with `ctx->user_mask` (the previously stored user cpumask) is there to further restrict the fallback mask, but when there is no overlap at all, the code already handles the empty case correctly by copying the full `cpus_allowed` mask. The `WARN_ON_ONCE` was overly conservative — it treated a benign race outcome as a bug-worthy condition.

The original reporter (Josh Don at Google) reproduced the warning by placing a process inside a cpuset cgroup, having one thread repeatedly switching the cpuset CPUs between `{1,2}` and `{1}`, while another thread repeatedly used `taskset` to set the process's affinity to CPU 2.

## Root Cause

The root cause is an incorrect assumption in commit `8f9ea86fdf99b` that the intersection of the cpuset-allowed mask and the stored `user_cpus_ptr` mask should never be empty at the point where the fallback logic executes in `__sched_setaffinity()`.

The vulnerable code path in `__sched_setaffinity()` (in `kernel/sched/syscalls.c`) proceeds as follows:

1. The caller requests affinity `new_mask` for a task (e.g., CPU 2).
2. `__set_cpus_allowed_ptr(p, ctx)` is called, which applies the affinity and stores the user-requested mask in `p->user_cpus_ptr` (via the `SCA_USER` flag in the affinity context).
3. `cpuset_cpus_allowed(p, cpus_allowed)` queries the current cpuset for the task's allowed CPUs.
4. The code checks `!cpumask_subset(new_mask, cpus_allowed)` — i.e., whether the requested mask is outside the cpuset.
5. If the requested mask is not a subset (meaning a cpuset update raced and shrank the allowed set), the fallback sets `new_mask = cpus_allowed`.
6. **The buggy code**: If `SCA_USER` is set and `ctx->user_mask` (the *old* user cpumask from a previous `sched_setaffinity` call) is non-NULL, the code intersects `new_mask` (now `cpus_allowed`) with `ctx->user_mask`:
   ```c
   bool empty = !cpumask_and(new_mask, new_mask, ctx->user_mask);
   if (WARN_ON_ONCE(empty))  // <-- BUG: spurious warning
       cpumask_copy(new_mask, cpus_allowed);
   ```
7. If the intersection is empty, the code correctly falls back to `cpus_allowed` again — but also fires a `WARN_ON_ONCE`.

The race that triggers this: between step 2 and step 3, another thread changes the cpuset from `{1,2}` to `{1}`. The first `__set_cpus_allowed_ptr()` in step 2 succeeds because the cpuset was still `{1,2}` when it ran, and it stores `user_cpus_ptr = {2}`. But by step 3, `cpuset_cpus_allowed()` returns `{1}`. Since `{2}` is not a subset of `{1}`, we enter the fallback. The old `ctx->user_mask` (from a *previous* call to `sched_setaffinity`) might also be `{2}`. So we compute `{1} AND {2} = {}` — an empty set — and `WARN_ON_ONCE` fires.

The fundamental error is that the `WARN_ON_ONCE` assumes the old user mask and current cpuset must always overlap. But since the cpuset can be changed concurrently and independently of the user's affinity requests, there is no guarantee of overlap. The race window is between the first `__set_cpus_allowed_ptr()` and the `cpuset_cpus_allowed()` call, during which the cpuset can change arbitrarily.

## Consequence

The primary consequence is a spurious kernel warning in dmesg. When `WARN_ON_ONCE` fires, it emits a full stack trace and warning message to the kernel log. While the scheduling behavior is functionally correct — the fallback to `cpus_allowed` still happens properly — the warning has several negative impacts:

1. **Noise and alarm**: The warning pollutes the kernel log and can trigger monitoring alerts in production environments. System administrators may investigate a non-issue.
2. **Panic on warn**: On systems configured with `kernel.panic_on_warn=1` (common in security-sensitive or test environments), this spurious warning causes an immediate kernel panic and system crash. This transforms a harmless race condition into a denial-of-service condition that can be trivially triggered by any user with access to cpuset cgroups and `sched_setaffinity()`.
3. **One-shot suppression**: Because `WARN_ON_ONCE` only fires once, the first occurrence of this benign race consumes the one-shot warning slot. If there were a genuine bug condition later that should trigger this path, it would be silently suppressed.

The warning is easily reproducible in production environments running containerized workloads where cpuset cgroups are frequently updated (e.g., Kubernetes pod scheduling, cgroup-based resource management) while tasks concurrently change their CPU affinity.

## Fix Summary

The fix is minimal and surgical: it replaces `WARN_ON_ONCE(empty)` with a plain `if (empty)` check in `__sched_setaffinity()` at the point where the intersection of `new_mask` and `ctx->user_mask` is computed.

```c
// Before (buggy):
if (WARN_ON_ONCE(empty))
    cpumask_copy(new_mask, cpus_allowed);

// After (fixed):
if (empty)
    cpumask_copy(new_mask, cpus_allowed);
```

The fix is correct because the empty intersection is a known and expected outcome of the race between cpuset updates and `sched_setaffinity()` calls. The fallback behavior (copying `cpus_allowed` into `new_mask` when the intersection is empty) is already the correct action. Removing the `WARN_ON_ONCE` eliminates the spurious warning while preserving the identical control flow and recovery logic. No functional behavior changes — only the removal of the diagnostic that was incorrectly classifying a benign race as a warning-worthy condition.

The fix was acked by Waiman Long (the author of the original commit that introduced the warning), tested by both Vincent Guittot and Madadi Vineeth Reddy, and merged by Peter Zijlstra.

## Triggering Conditions

The following precise conditions are required to trigger the bug:

- **Kernel version**: v6.2 through v6.13-rc2 (any kernel containing commit `8f9ea86fdf99b` but not the fix `70ee7947a290`).
- **CPU count**: At least 3 CPUs (CPU 0 for the driver, CPUs 1 and 2 for the cpuset and affinity).
- **Cgroup v2 with cpuset controller**: A cpuset cgroup must be configured and operational.
- **A task inside a cpuset cgroup**: The target task must be a member of a cpuset cgroup whose `cpuset.cpus` will be modified.
- **Concurrent cpuset modification**: One thread must repeatedly modify the cpuset's allowed CPUs, alternating between a set that includes the target CPU (e.g., `{1,2}`) and a set that excludes it (e.g., `{1}`).
- **Concurrent sched_setaffinity**: Another thread must repeatedly call `sched_setaffinity()` on the target task, requesting a CPU that is only intermittently in the cpuset (e.g., CPU 2).
- **Race timing**: The cpuset must shrink (e.g., from `{1,2}` to `{1}`) during the window between the first `__set_cpus_allowed_ptr()` call and the `cpuset_cpus_allowed()` query within `__sched_setaffinity()`. This is a narrow but frequently hittable window when both operations loop tightly.
- **Prior user_cpus_ptr**: The task must have a non-NULL `user_cpus_ptr` from a previous `sched_setaffinity()` call. This is satisfied after the first `sched_setaffinity()` call in the loop.
- **SCA_USER flag**: The affinity change must come through the `sched_setaffinity()` path (not `set_cpus_allowed_ptr()`), as only `sched_setaffinity()` sets the `SCA_USER` flag and allocates/swaps `user_cpus_ptr`.

The race is probabilistic but highly reproducible with tight loops. The original author reports it triggers reliably with the simple setup of two threads in a tight loop.

## Reproduce Strategy (kSTEP)

The strategy is to reproduce the race between `sched_setaffinity()` and cpuset modification using kSTEP's cgroup and kthread facilities, combined with `KSYM_IMPORT` to call `sched_setaffinity()` directly from kernel module context.

### Step 1: Setup topology and task

Configure QEMU with at least 3 CPUs (CPU 0 for the driver, CPUs 1-2 for the test). Create a CFS task using `kstep_task_create()` and wake it up. This task will be the target whose affinity is modified.

### Step 2: Create cpuset cgroup

Use `kstep_cgroup_create("test_cpuset")` to create a cpuset cgroup. Set the initial cpuset to CPUs 1-2 via `kstep_cgroup_set_cpuset("test_cpuset", "1-2")`. Add the target task to this cgroup with `kstep_cgroup_add_task("test_cpuset", target_pid)`.

### Step 3: Import sched_setaffinity and warn detection

Use `KSYM_IMPORT(sched_setaffinity)` to import the `sched_setaffinity()` function. Also import a mechanism to detect whether a kernel warning fired. Options include:
- `KSYM_IMPORT(warn_count)` to read the kernel's global `warn_count` atomic counter (defined in `kernel/panic.c` on newer kernels).
- Alternatively, read `/proc/sys/kernel/warn_count` via `kstep_write`/kernel file I/O before and after the test.
- As another option, set `kernel.panic_on_warn=1` via `kstep_sysctl_write("kernel.panic_on_warn", "%d", 1)` to make the WARN cause a panic (the test passes if the buggy kernel panics, fails if it doesn't — though this makes the kstep_pass/kstep_fail output impossible on the buggy kernel, so the "warn_count" approach is preferred).

### Step 4: Create racing kthreads

Create two kthreads using `kstep_kthread_create()`:

**Kthread A (cpuset modifier)**: Binds to CPU 1. In a tight loop, alternates the cpuset between `"1-2"` and `"1"` by repeatedly calling `kstep_cgroup_set_cpuset("test_cpuset", "1-2")` and `kstep_cgroup_set_cpuset("test_cpuset", "1")`. Runs for a fixed number of iterations (e.g., 10,000).

**Kthread B (affinity setter)**: Binds to CPU 2. In a tight loop, calls `sched_setaffinity(target_pid, &mask_cpu2)` where `mask_cpu2 = {2}`. Runs for the same number of iterations.

Both kthreads start simultaneously via `kstep_kthread_start()`.

### Step 5: Execute and detect

Start both kthreads and let them run concurrently. The driver's main thread can use `kstep_tick_repeat()` or `kstep_sleep()` to wait for the kthreads to complete. After the kthreads finish their iterations (or after a timeout), read the `warn_count` to check if any warnings fired.

### Step 6: Pass/fail criteria

- **On the buggy kernel (pre-fix)**: After the race loop, `warn_count` should have increased (or if using `panic_on_warn`, the kernel panics). Report `kstep_fail("WARN_ON_ONCE triggered: warn_count increased from %d to %d")`.
- **On the fixed kernel (post-fix)**: `warn_count` should remain unchanged. The race still occurs but the `if (empty)` path simply falls back silently without any warning. Report `kstep_pass("No spurious warning triggered after %d iterations")`.

### Step 7: kSTEP extensions needed

The following minor extensions or workarounds are needed:

1. **`KSYM_IMPORT(sched_setaffinity)`**: Required to call `sched_setaffinity()` from kernel module context. This function is not called by any existing kSTEP API (`kstep_task_pin` uses `set_cpus_allowed_ptr` which does NOT go through `__sched_setaffinity` and does NOT set the `SCA_USER` flag). The `KSYM_IMPORT` mechanism already exists in kSTEP for accessing internal kernel symbols.

2. **Warning detection**: Need to detect `WARN_ON_ONCE` firing. The simplest approach is `KSYM_IMPORT(warn_count)` to read the global atomic warning counter. Alternatively, `kstep_sysctl_write("kernel.panic_on_warn", "%d", 1)` could be used, but this crashes the kernel on the buggy path, preventing clean output.

3. **Kthread loop body**: The kthreads need to execute custom code in a tight loop. The existing `kstep_kthread_create()` creates kthreads with a predefined body (block/wake pattern). For this bug, we need kthreads that execute custom functions (cpuset write and sched_setaffinity calls) in a loop. This may require defining the kthread functions directly in the driver using standard kernel kthread APIs (`kthread_run()`) rather than kSTEP's wrapper, or extending kSTEP's kthread API to accept a custom function pointer.

### Expected behavior summary

| Kernel    | warn_count delta | Behavior |
|-----------|-----------------|----------|
| Buggy     | > 0             | WARN_ON_ONCE fires on first empty intersection; stack trace in dmesg |
| Fixed     | 0               | Empty intersection handled silently; no warning, same fallback logic |

The race should be reliably triggerable within a few thousand iterations given two CPUs running tight loops, as the race window (between `__set_cpus_allowed_ptr()` and `cpuset_cpus_allowed()` in `__sched_setaffinity()`) is small but frequently hit under contention.
