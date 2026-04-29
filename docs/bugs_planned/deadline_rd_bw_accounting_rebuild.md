# Deadline: Root Domain Bandwidth Accounting Not Rebuilt After Domain Changes

**Commit:** `2ff899e3516437354204423ef0a94994717b8e6a`
**Affected files:** kernel/sched/deadline.c, kernel/sched/topology.c, kernel/sched/core.c, kernel/cgroup/cpuset.c
**Fixed in:** v6.15-rc1
**Buggy since:** v6.14-rc1 (introduced by commit `53916d5fd3c0` "sched/deadline: Check bandwidth overflow earlier for hotplug")

## Bug Description

The SCHED_DEADLINE bandwidth accounting information (`total_bw` in `struct root_domain`'s `dl_bw`) is not properly rebuilt when scheduling domains are changed via the `partition_sched_domains()` path. This path is taken during CPU suspend/resume operations and was introduced/made worse by commit `53916d5fd3c0` which restructured the hotplug handling for DEADLINE bandwidth.

The function `dl_rebuild_rd_accounting()`, which is responsible for clearing all root domain bandwidth counters and then re-adding the bandwidth of every SCHED_DEADLINE task (and dl_server), was only called from `partition_and_rebuild_sched_domains()` in `kernel/cgroup/cpuset.c`. However, the suspend/resume code paths in `kernel/sched/core.c` (`cpuset_cpu_active()` and `cpuset_cpu_inactive()`) call `partition_sched_domains(1, NULL, NULL)` directly, bypassing the cpuset wrapper entirely. This means domain rebuilds triggered by suspend/resume never invoke `dl_rebuild_rd_accounting()`.

Additionally, the old `dl_rebuild_rd_accounting()` had a bug where it only cleared the default root domain (`def_root_domain`) instead of all root domains. Systems with cpuset partitions or `isolcpus` boot parameter have non-default root domains whose bandwidth accounting was never properly cleared during a rebuild, leading to stale or double-counted bandwidth values.

The bug was reported by Jon Hunter on an NVIDIA Tegra (aarch64) board configured with `isolcpus`, where suspend/resume would fail because the DEADLINE bandwidth checks would detect an overflow condition due to corrupted `total_bw` values after resume.

## Root Cause

The root cause is a missing call to `dl_rebuild_rd_accounting()` in the `partition_sched_domains_locked()` function in `kernel/sched/topology.c`. Before the fix, the function only rebuilt scheduling domains (calling `build_sched_domains()` and `detach_destroy_domains()`) but did not restore DEADLINE bandwidth accounting.

The call chain for the buggy path is:

1. `sched_cpu_activate()` → `cpuset_cpu_active()` → `partition_sched_domains(1, NULL, NULL)` → `partition_sched_domains_locked()` — **no dl_rebuild_rd_accounting() called**
2. `sched_cpu_deactivate()` → `cpuset_cpu_inactive()` → `partition_sched_domains(1, NULL, NULL)` → `partition_sched_domains_locked()` — **no dl_rebuild_rd_accounting() called**

Compare with the normal cpuset rebuild path:

3. `rebuild_sched_domains_locked()` → `partition_and_rebuild_sched_domains()` → `partition_sched_domains_locked()` + `dl_rebuild_rd_accounting()` — **correctly called**

When `partition_sched_domains_locked()` rebuilds domains, `init_rootdomain()` initializes `dl_bw.total_bw = 0` for newly created root domains. Without `dl_rebuild_rd_accounting()`, the bandwidth from existing SCHED_DEADLINE tasks is never re-added. This means `total_bw` understates the actual committed bandwidth.

The second part of the root cause is in `dl_rebuild_rd_accounting()` itself. The old code cleared only `def_root_domain`:

```c
dl_clear_root_domain(&def_root_domain);
```

This is incorrect when multiple root domains exist (e.g., due to `isolcpus` or cpuset partitions). Non-default root domains' `total_bw` would never be cleared, potentially leading to stale values being accumulated when tasks are re-added.

The third part involves `dl_clear_root_domain()` itself. The old implementation directly added dl_server bandwidth with a simple assignment:

```c
rd->dl_bw.total_bw += dl_se->dl_bw;
```

This does not properly update the `extra_bw` field on each CPU's `dl_rq`, which should be adjusted when bandwidth is added or removed via `__dl_add()` / `__dl_sub()`. The `extra_bw` field tracks the surplus bandwidth available for reclamation by SCHED_DEADLINE tasks.

## Consequence

The primary consequence is that SCHED_DEADLINE admission control becomes incorrect after suspend/resume or any operation that triggers `partition_sched_domains()` directly. Specifically:

1. **Suspend/resume failure:** On systems with `isolcpus` and SCHED_DEADLINE tasks (including dl_servers), the corrupted `total_bw` can cause `__dl_overflow()` to return true erroneously during `dl_bw_deactivate()` checks in the CPU hotplug path. This triggers a rollback of the CPU offline operation, causing suspend to fail. Jon Hunter reported this exact scenario on an NVIDIA Tegra board, where the system could not suspend because DEADLINE bandwidth checks falsely detected an overflow.

2. **Bandwidth over-subscription:** If `total_bw` is understated (because task bandwidth was not re-added after domain rebuild), new SCHED_DEADLINE tasks could pass admission control when they shouldn't, leading to CPU overload. With more SCHED_DEADLINE bandwidth committed than available, tasks miss their deadlines, leading to potential real-time guarantee violations.

3. **Stale extra_bw values:** The `extra_bw` field on per-CPU `dl_rq` structures is not properly updated when dl_server bandwidth is restored, leading to incorrect reclamation bandwidth calculations. This affects SCHED_DEADLINE tasks using the GRUB (Greedy Reclamation of Unused Bandwidth) algorithm.

## Fix Summary

The fix makes three key changes:

**1. Move `dl_rebuild_rd_accounting()` into `partition_sched_domains_locked()`:** The call is placed at the very end of `partition_sched_domains_locked()` in `kernel/sched/topology.c`, after all domain rebuilds are complete and the new domain configuration is stable. This ensures that EVERY code path that rebuilds scheduling domains — whether through the cpuset rebuild path, the suspend/resume path, or any other caller of `partition_sched_domains()` — will always rebuild DEADLINE bandwidth accounting. The corresponding call in `partition_and_rebuild_sched_domains()` is removed since it is now redundant.

**2. Clear ALL root domains, not just `def_root_domain`:** The `dl_rebuild_rd_accounting()` function is changed to iterate over all possible CPUs using the `dl_bw_visited()` cookie mechanism (generalized by an earlier patch in the series) to visit each unique root domain exactly once and call `dl_clear_root_domain_cpu()` on it. A new wrapper `dl_clear_root_domain_cpu(int cpu)` is added to look up a CPU's root domain and clear it. This replaces the old approach of only clearing `def_root_domain`.

**3. Use `__dl_add()` in `dl_clear_root_domain()`:** When restoring dl_server bandwidth in `dl_clear_root_domain()`, the fix uses `__dl_add(&rd->dl_bw, dl_se->dl_bw, dl_bw_cpus(i))` instead of directly incrementing `rd->dl_bw.total_bw`. This ensures that `extra_bw` on each CPU's `dl_rq` is properly updated via `__dl_update()`, maintaining consistency across all bandwidth-related fields.

Additionally, a `cpuset_reset_sched_domains()` wrapper is added that acquires `cpuset_mutex` before calling `partition_sched_domains(1, NULL, NULL)`. The suspend/resume code in `core.c` is changed to use this wrapper instead of calling `partition_sched_domains()` directly, ensuring that `dl_rebuild_rd_accounting()` (which asserts `cpuset_mutex` is held) always has the required lock.

## Triggering Conditions

The bug requires the following conditions to trigger:

- **Kernel version:** v6.14-rc1 through v6.14 (introduced by `53916d5fd3c0`, fixed by `2ff899e3516437354204423ef0a94994717b8e6a` in v6.15-rc1). However, the underlying accounting issues with `dl_clear_root_domain()` using simple addition instead of `__dl_add()` may have existed longer.

- **CONFIG_SMP=y** and **CONFIG_CPUSETS=y**: The bug is in the SMP domain rebuild path, and cpuset support is needed for non-default root domains.

- **Multiple root domains:** The system must have more than one root domain to expose the "only clears def_root_domain" part of the bug. This is achieved via `isolcpus=` boot parameter or cpuset v2 partitions. Even with a single root domain, the missing `dl_rebuild_rd_accounting()` call is still a problem (task bandwidth is lost), but the `def_root_domain`-only clearing bug is not observable.

- **SCHED_DEADLINE tasks or active dl_servers:** There must be bandwidth to account. Modern kernels (v6.6+) have dl_servers enabled by default on each CPU's `fair_server`, so even without explicit SCHED_DEADLINE tasks, there is dl_server bandwidth to misaccount. The bug is more visible with explicit SCHED_DEADLINE tasks that have significant bandwidth reservations.

- **Domain rebuild via direct `partition_sched_domains()` call:** The trigger is a scheduling domain rebuild that goes through `partition_sched_domains()` or `partition_sched_domains_locked()` directly, rather than through the cpuset `partition_and_rebuild_sched_domains()` wrapper. The primary real-world trigger is CPU suspend/resume (the `cpuset_cpu_active()` and `cpuset_cpu_inactive()` functions).

- **Multi-CPU system:** At least 2 CPUs are needed (one for the housekeeping domain, one for the isolated domain). Jon Hunter's Tegra board had multiple CPUs with some isolated.

- **Reliability:** The bug is deterministic once the conditions are met — every suspend/resume cycle on an affected system will trigger the incorrect accounting. There is no timing-dependent race condition; the issue is simply a missing function call.

## Reproduce Strategy (kSTEP)

This bug can be reproduced in kSTEP with minor framework extensions. The core insight is that the bug is in `partition_sched_domains_locked()` not calling `dl_rebuild_rd_accounting()`, and we can trigger this code path by calling `partition_sched_domains()` directly via `KSYM_IMPORT`.

### Required kSTEP Extensions

1. **SCHED_DEADLINE task creation:** Add a `kstep_task_dl(struct task_struct *p, u64 runtime_ns, u64 deadline_ns, u64 period_ns)` helper that calls `sched_setattr_nocheck()` (or the equivalent internal function) to set a task's scheduling class to `SCHED_DEADLINE` with the specified parameters. This is needed to have tasks with explicit DEADLINE bandwidth that gets tracked in `total_bw`. Alternatively, the dl_servers that exist by default on each CPU's `fair_server` already contribute bandwidth, so DL task creation may be optional for a minimal reproducer.

2. **Direct `partition_sched_domains()` call:** Use `KSYM_IMPORT(partition_sched_domains)` to import the function and call it directly. Also import `cpuset_mutex` for proper locking if needed.

### Driver Setup

```
Number of CPUs: 4 (QEMU configured with -smp 4)
Topology: default (no special SMT/cluster needed)
```

1. **Create cpuset partitions to get multiple root domains:**
   ```c
   kstep_cgroup_create("group_a");
   kstep_cgroup_set_cpuset("group_a", "1-2");
   kstep_cgroup_create("group_b");
   kstep_cgroup_set_cpuset("group_b", "3");
   ```
   This creates non-default root domains for group_a (CPUs 1-2) and group_b (CPU 3). CPU 0 remains in the default root domain (used by the driver).

2. **Create SCHED_DEADLINE tasks (if DL task extension is added):**
   ```c
   struct task_struct *dl_task = kstep_task_create();
   kstep_task_pin(dl_task, 1, 2);  // Pin to CPUs 1-2
   kstep_task_dl(dl_task, 5000000, 10000000, 10000000);  // 5ms/10ms = 50% bandwidth
   kstep_cgroup_add_task("group_a", dl_task->pid);
   ```

3. **Let the system stabilize:** Run a few ticks so the task's bandwidth is accounted.
   ```c
   kstep_tick_repeat(20);
   ```

### Bug Triggering Sequence

4. **Record pre-rebuild bandwidth:** Read `total_bw` from the root domain of CPU 1 (group_a's domain):
   ```c
   struct rq *rq1 = cpu_rq(1);
   u64 pre_total_bw = rq1->rd->dl_bw.total_bw;
   ```

5. **Simulate the suspend path by calling `partition_sched_domains(1, NULL, NULL)` directly:**
   ```c
   KSYM_IMPORT(partition_sched_domains);
   partition_sched_domains(1, NULL, NULL);
   ```
   This collapses all domains into a single default root domain, just as `cpuset_cpu_active()` or `cpuset_cpu_inactive()` does during suspend/resume. On the buggy kernel, `dl_rebuild_rd_accounting()` is NOT called by this path.

6. **Rebuild domains to restore the cpuset partitions:**
   ```c
   KSYM_IMPORT(rebuild_sched_domains);
   rebuild_sched_domains();
   ```
   This goes through the cpuset path and DOES call `dl_rebuild_rd_accounting()` (even on the buggy kernel, this path has the call). However, the intermediate state after step 5 has already corrupted accounting.

   Alternatively, to fully demonstrate the bug, skip step 6 and instead examine the single root domain after step 5. On the buggy kernel, `total_bw` on the single domain should be missing the DL task's bandwidth contribution because `dl_rebuild_rd_accounting()` was never called after the domain collapse.

### Detection Criteria

7. **Check bandwidth accounting after the domain collapse (after step 5, before step 6):**
   ```c
   struct rq *rq1 = cpu_rq(1);
   u64 post_total_bw = rq1->rd->dl_bw.total_bw;
   ```

   **On buggy kernel:** After `partition_sched_domains(1, NULL, NULL)`, all CPUs are in `def_root_domain`. The `total_bw` of `def_root_domain` will NOT include the DL task's bandwidth (because `dl_rebuild_rd_accounting()` was not called). It will only include dl_server bandwidth that was added during domain initialization. If we created a DL task with 50% bandwidth, this bandwidth will be missing from `total_bw`.

   **On fixed kernel:** After `partition_sched_domains(1, NULL, NULL)`, `dl_rebuild_rd_accounting()` is called automatically. It clears all root domains, re-adds dl_server bandwidth via `__dl_add()`, and iterates all cpusets to re-add DL task bandwidth. The `total_bw` will correctly include all DL task and dl_server contributions.

8. **Pass/fail criteria:**
   ```c
   if (post_total_bw < pre_total_bw - some_tolerance) {
       kstep_fail("total_bw dropped from %llu to %llu after domain rebuild — DL bandwidth lost",
                  pre_total_bw, post_total_bw);
   } else {
       kstep_pass("total_bw correctly maintained: pre=%llu post=%llu",
                  pre_total_bw, post_total_bw);
   }
   ```

### Alternative Minimal Reproducer (dl_server only, no DL task extension needed)

Even without creating explicit SCHED_DEADLINE tasks, the bug can be observed through dl_server bandwidth and the `extra_bw` field:

1. Set up 4 CPUs with cpuset partitions as above.
2. Record `total_bw` and `extra_bw` values for each CPU's root domain.
3. Call `partition_sched_domains(1, NULL, NULL)` via KSYM_IMPORT.
4. Check `extra_bw` on each CPU's `dl_rq`.

On the buggy kernel, `dl_clear_root_domain()` uses `total_bw += dl_se->dl_bw` (simple addition) instead of `__dl_add()`, so `extra_bw` will not be properly updated. On the fixed kernel, `__dl_add()` distributes the negative `extra_bw` adjustment across all active CPUs in the domain.

### Callbacks

Use `on_tick_begin` to log `total_bw`, `extra_bw`, and the current root domain pointer for each CPU at each tick, providing visibility into the accounting state changes during the domain rebuild sequence.

### Expected Kernel Version Guard

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(6, 15, 0)
```

The bug was introduced in v6.14-rc1 (by `53916d5fd3c0`) and fixed in v6.15-rc1, so the driver should target v6.14.x kernels.
