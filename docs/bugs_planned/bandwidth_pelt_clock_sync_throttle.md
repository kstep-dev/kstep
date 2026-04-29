# Bandwidth: Missing pelt_clock_throttled Sync for New Cgroups on Throttled Hierarchy

**Commit:** `0e4a169d1a2b630c607416d9e3739d80e176ed67`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.18-rc3
**Buggy since:** v6.18-rc1 (introduced by commit `e1fad12dcb66` "sched/fair: Switch to task based throttle model", which shipped in v6.18-rc1, and then exposed by commit `fe8d238e646e` "sched/fair: Propagate load for throttled cfs_rq" which changed `propagate_entity_cfs_rq()` to use `cfs_rq_pelt_clock_throttled()` instead of `cfs_rq_throttled()`/`throttled_hierarchy()`)

## Bug Description

When a new cgroup is created within a CFS bandwidth-throttled hierarchy, the `sync_throttle()` function is called during `online_fair_sched_group()` to synchronize the new cgroup's `cfs_rq` throttle state with its parent. However, `sync_throttle()` only copied `throttle_count` and `throttled_clock_pelt` from the parent — it failed to set the `pelt_clock_throttled` flag on the new `cfs_rq`. This created an inconsistency where a child `cfs_rq` in a throttled hierarchy had `pelt_clock_throttled = 0` while its parent had `pelt_clock_throttled = 1`.

This inconsistency became a problem after commit `fe8d238e646e` ("sched/fair: Propagate load for throttled cfs_rq") changed `propagate_entity_cfs_rq()` to use `cfs_rq_pelt_clock_throttled()` (which checks the `pelt_clock_throttled` flag) instead of the old `cfs_rq_throttled()`/`throttled_hierarchy()` checks for deciding whether to add a `cfs_rq` to the leaf `cfs_rq` list. Because the newly created child `cfs_rq` falsely reported `pelt_clock_throttled = 0`, `propagate_entity_cfs_rq()` would attempt to add it to the leaf list via `list_add_leaf_cfs_rq()`, but since the parent was throttled and not on the list, the branch could not be connected to the tree. This left a dangling reference in `rq->tmp_alone_branch`, which would later trigger the `assert_list_leaf_cfs_rq()` warning.

The bug manifests as a `WARN_ON_ONCE` in `enqueue_task_fair()` at the `assert_list_leaf_cfs_rq()` check, which verifies that `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list` after enqueuing is complete. This assertion ensures that no orphaned branches exist in the leaf `cfs_rq` list.

## Root Cause

The root cause is a missing initialization of `cfs_rq->pelt_clock_throttled` in the `sync_throttle()` function. When a new task group is brought online via `online_fair_sched_group()`, the sequence is:

1. `attach_entity_cfs_rq(se)` is called, which calls `propagate_entity_cfs_rq(se)`.
2. `sync_throttle(tg, cpu)` is called.

The problem lies in step 2. Before the fix, `sync_throttle()` set:
```c
cfs_rq->throttle_count = pcfs_rq->throttle_count;
cfs_rq->throttled_clock_pelt = rq_clock_pelt(cpu_rq(cpu));
```
But it did **not** set `cfs_rq->pelt_clock_throttled`. Since `cfs_rq` memory is zeroed on allocation, `pelt_clock_throttled` remained `0` even when the parent's `pelt_clock_throttled` was `1`.

However, the actual problem manifests in step 1, which occurs **before** `sync_throttle()`. When `attach_entity_cfs_rq()` calls `propagate_entity_cfs_rq()`, the new child's `pelt_clock_throttled` is still `0` (it hasn't been synced yet). The function then enters this code path:

```c
if (!cfs_rq_pelt_clock_throttled(cfs_rq))  // returns false (0), so we enter
    list_add_leaf_cfs_rq(cfs_rq);          // tries to add child to leaf list
```

Inside `list_add_leaf_cfs_rq()`, the child `cfs_rq` is not on the list (`on_list = 0`), its parent `cfs_rq` (cgroup B in the commit message example) is also not on the list (because it was throttled and had its PELT frozen). So the function falls through to the "parent not on list" case (line 369) and adds the `cfs_rq` to `rq->tmp_alone_branch`, updating `tmp_alone_branch` to point to the child's `leaf_cfs_rq_list`:

```c
list_add_rcu(&cfs_rq->leaf_cfs_rq_list, rq->tmp_alone_branch);
rq->tmp_alone_branch = &cfs_rq->leaf_cfs_rq_list;
return false;  // branch NOT connected to tree
```

When the `for_each_sched_entity(se)` loop in `propagate_entity_cfs_rq()` continues to the parent (cgroup B), the parent's `pelt_clock_throttled` IS set to `1`, so:

```c
if (!cfs_rq_pelt_clock_throttled(cfs_rq))  // returns true (throttled), so we SKIP
    list_add_leaf_cfs_rq(cfs_rq);          // parent is NOT added
```

The parent is skipped. The loop continues to root, which is already on the list. `list_add_leaf_cfs_rq()` for root returns early because `on_list = 1`, and it returns `rq->tmp_alone_branch == &rq->leaf_cfs_rq_list` — which is **false**, because `tmp_alone_branch` still points to the child's `leaf_cfs_rq_list`.

This leaves `rq->tmp_alone_branch` in a dangling state, pointing to the newly created child `cfs_rq`'s list entry rather than being reset to `&rq->leaf_cfs_rq_list`.

Later, when a completely unrelated task is woken up in a different subtree (e.g., cgroup A) whose entire hierarchy is already on the leaf list, `list_add_leaf_cfs_rq()` keeps returning early (because `on_list` is already set for all cfs_rqs in that path), and nobody resets `tmp_alone_branch`. The final `assert_list_leaf_cfs_rq()` at the end of `enqueue_task_fair()` then fires the `WARN_ON_ONCE` because `tmp_alone_branch` does not equal `&rq->leaf_cfs_rq_list`.

## Consequence

The observable impact is a kernel `WARN_ON_ONCE` warning at `kernel/sched/fair.c:400` from `enqueue_task_fair()`, triggered during task wakeup (`try_to_wake_up`) or process exit (`do_exit -> sched_move_task`). The warning message is:

```
WARNING: CPU: 0 PID: 1 at kernel/sched/fair.c:400 enqueue_task_fair+0x925/0x980
```

The stack trace from Matteo's report shows the `try_to_wake_up` path:
```
enqueue_task_fair -> enqueue_task -> ttwu_do_activate -> try_to_wake_up
```

Aaron Lu was able to reproduce it via the `sched_move_task` path during process exit:
```
enqueue_task_fair -> enqueue_task -> sched_move_task -> do_exit
```

Beyond the warning, the corrupted `tmp_alone_branch` pointer means the leaf `cfs_rq` list is in an inconsistent state. The leaf `cfs_rq` list is used during load balancing and PELT load propagation to iterate over all cfs_rqs that have pending load to decay. A broken list could lead to:
- Missing load decay for some cfs_rqs, causing inaccurate load tracking and unfair CPU time distribution.
- Potential list corruption if the dangling `tmp_alone_branch` reference persists across multiple enqueue/dequeue cycles.
- In the worst case, use-after-free if the orphaned cfs_rq is later destroyed while still referenced by `tmp_alone_branch`.

## Fix Summary

The fix adds two changes to `kernel/sched/fair.c`:

**1. Set `pelt_clock_throttled = 1` for new cfs_rqs joining a throttled hierarchy (in `sync_throttle()`):**

```c
if (cfs_rq->throttle_count)
    cfs_rq->pelt_clock_throttled = 1;
```

Rather than simply syncing with `pcfs_rq->pelt_clock_throttled`, the fix unconditionally sets `pelt_clock_throttled = 1` when `throttle_count > 0`. This is more robust than copying the parent's value because the parent's PELT clock state may change independently (e.g., via dequeue on other subtrees) without updating this new child. Since the new cfs_rq has no tasks and no pending load, starting with PELT frozen is safe — the first enqueue or bandwidth distribution will unfreeze it and add it to the leaf list properly.

**2. Add `assert_list_leaf_cfs_rq()` at the end of `propagate_entity_cfs_rq()`:**

```c
assert_list_leaf_cfs_rq(rq_of(cfs_rq));
```

This is a defensive assertion to catch any future cases where `propagate_entity_cfs_rq()` leaves `tmp_alone_branch` in a dangling state. It ensures the invariant that after propagation is complete, `tmp_alone_branch` has been reset to `&rq->leaf_cfs_rq_list`.

The fix is correct because: (a) newly created cfs_rqs on a throttled hierarchy have no tasks and no load, so having their PELT clock frozen is harmless; (b) when actual work arrives (task enqueue or bandwidth distribution/unthrottle), the PELT clock will be properly unfrozen; (c) with `pelt_clock_throttled = 1`, `propagate_entity_cfs_rq()` will correctly skip adding this cfs_rq to the leaf list, avoiding the dangling `tmp_alone_branch` problem entirely.

## Triggering Conditions

The following conditions are all required to trigger the bug:

1. **CFS bandwidth throttling must be active:** At least one cgroup must have `cpu.max` configured with a quota (e.g., `25000 100000` for 25% CPU). The `CONFIG_CFS_BANDWIDTH` kernel config must be enabled.

2. **Task-based throttle model:** The bug exists only in kernels with commit `e1fad12dcb66` ("sched/fair: Switch to task based throttle model") and commit `fe8d238e646e` ("sched/fair: Propagate load for throttled cfs_rq"). Both landed in the v6.18-rc1 merge window.

3. **A cgroup in the hierarchy must be throttled with PELT frozen:** The parent cgroup must be in a state where `pelt_clock_throttled = 1`. This happens when a cfs_rq is throttled and has no running/queued tasks, causing the PELT clock to be frozen.

4. **A new child cgroup must be created under the throttled parent:** When `online_fair_sched_group()` runs for the new cgroup, `sync_throttle()` fails to set `pelt_clock_throttled`, and `attach_entity_cfs_rq()` → `propagate_entity_cfs_rq()` causes the dangling `tmp_alone_branch`.

5. **A subsequent task wakeup/enqueue must occur on the same CPU in a different subtree that is already fully on the leaf list:** This enqueue walks up a path where all cfs_rqs have `on_list = 1`, so `list_add_leaf_cfs_rq()` returns early without resetting `tmp_alone_branch`. The final `assert_list_leaf_cfs_rq()` fires.

The trigger scenario from Matteo's report: systemd creates user session cgroups under `user.slice` which has `CPUQuota=25%` and is pinned to CPU 0. During SSH login, systemd spawns new processes and creates new cgroups. When `user.slice`'s bandwidth is exhausted, it gets throttled. If a new child cgroup is created (or a task is moved into a child cgroup) while the parent is throttled, the bug triggers. The probability increases with:
- Lower CPU quota (e.g., 10% instead of 25%)
- Higher background load (e.g., stress-ng running)
- Multiple SSH logins in quick succession during boot

Matteo reported being able to reproduce "about once or twice every 10 SSH executions" with 10% CPU quota and background stress.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by simulating the exact sequence of events: creating a cgroup hierarchy with CFS bandwidth throttling, causing the parent cgroup to be throttled with PELT frozen, then creating a new child cgroup under the throttled parent, and finally waking up a task in a different subtree.

### Step-by-step plan:

1. **CPU Configuration:**
   - Configure QEMU with 2 CPUs. The driver runs on CPU 0; tasks and cgroups target CPU 1.

2. **Cgroup Hierarchy Setup:**
   Create a cgroup hierarchy that resembles:
   ```
   root
    ├── A (unthrottled, has a task on the leaf list)
    └── B (will be throttled with bandwidth limit)
        └── C (will be created dynamically while B is throttled)
   ```

   - Create parent cgroup "B" with CFS bandwidth limits using `kstep_cgroup_create("B")`. Note: kSTEP currently supports `kstep_cgroup_set_weight()` and `kstep_cgroup_set_cpuset()` but may need an extension to set `cpu.max` (CFS bandwidth). See below for the required kSTEP extension.
   - Create cgroup "A" with `kstep_cgroup_create("A")`.

3. **kSTEP Extension Required — CFS Bandwidth Control:**
   kSTEP needs a new helper to write to the `cpu.max` cgroup interface file to set bandwidth limits. This could be implemented as:
   ```c
   kstep_cgroup_set_cpu_max(const char *name, long quota_us, long period_us)
   ```
   This would write `quota_us period_us` to `/sys/fs/cgroup/<name>/cpu.max`. Alternatively, `kstep_sysctl_write()` could be extended or a generic `kstep_cgroup_write()` helper added. The implementation would use `cgroup_file_write()` or the task group's `tg_set_cfs_quota()` / `tg_set_cfs_period()` internal APIs.

   If `kstep_sysctl_write` can be adapted to write cgroup files, or if a `kstep_cgroup_set_bandwidth(name, quota, period)` is added, this is a minor extension.

4. **Task Creation and Placement:**
   - Create task `t_a` (CFS) and add it to cgroup "A": `kstep_task_create()`, `kstep_cgroup_add_task("A", t_a->pid)`, `kstep_task_pin(t_a, 1, 1)` (pin to CPU 1).
   - Create task `t_b` (CFS) and add it to cgroup "B": `kstep_task_create()`, `kstep_cgroup_add_task("B", t_b->pid)`, `kstep_task_pin(t_b, 1, 1)`.
   - Wake up both tasks so they become runnable on CPU 1.

5. **Trigger Throttling on Cgroup B:**
   - Use `kstep_tick_repeat()` to advance ticks so that cgroup B exhausts its bandwidth quota. The CFS bandwidth period timer fires every `period` (default 100ms = 100 ticks at HZ=1000). With a quota of e.g. 10ms (10000us), running t_b for more than 10 ticks within a period should trigger throttling.
   - Verify that cgroup B's cfs_rq is throttled by checking `cfs_rq->throttled` or `cfs_rq_throttled()`.
   - After throttling, block task `t_b` with `kstep_task_block(t_b)` so there are no running/queued tasks in B's hierarchy, causing `pelt_clock_throttled` to be set to `1`.

6. **Create New Child Cgroup Under Throttled B:**
   - While cgroup B is throttled and has its PELT clock frozen, dynamically create a new child cgroup "C" under "B": `kstep_cgroup_create("C")` (as a child of "B"). Note: kSTEP may need to support hierarchical cgroup creation — creating "B/C" as a subdirectory of "B". This might require a small extension like `kstep_cgroup_create_child("B", "C")` or using path notation `kstep_cgroup_create("B/C")`.
   
   When this child is created, the kernel calls `online_fair_sched_group()` which calls `attach_entity_cfs_rq()` → `propagate_entity_cfs_rq()` → `list_add_leaf_cfs_rq()`. On the buggy kernel, `pelt_clock_throttled` is `0` for the new child, causing it to be incorrectly added to `tmp_alone_branch`.

7. **Trigger the Assert by Waking a Task in Cgroup A:**
   - Wake up task `t_a` in cgroup A (if it was blocked) or enqueue a new task into cgroup A: `kstep_task_wakeup(t_a)`.
   - Since cgroup A's entire hierarchy (A's cfs_rq and root cfs_rq) is already on the leaf list, `list_add_leaf_cfs_rq()` returns early for each level without resetting `tmp_alone_branch`.
   - The `assert_list_leaf_cfs_rq()` at the end of `enqueue_task_fair()` (line 7056) will fire because `rq->tmp_alone_branch != &rq->leaf_cfs_rq_list`.

8. **Detection / Pass-Fail Criteria:**
   - **On the buggy kernel:** Use `on_tick_begin` or check the kernel log for the `WARN_ON_ONCE` from `assert_list_leaf_cfs_rq()`. Alternatively, use `KSYM_IMPORT` to access `cpu_rq(1)->tmp_alone_branch` and compare it to `&cpu_rq(1)->leaf_cfs_rq_list` after the cgroup creation step. If they differ, the bug is reproduced. Call `kstep_fail("tmp_alone_branch dangling: %px != %px")`.
   - **On the fixed kernel:** After creating cgroup "C" under throttled "B", the new child's `pelt_clock_throttled` is set to `1` by `sync_throttle()`. `propagate_entity_cfs_rq()` will skip adding it to the leaf list, and `tmp_alone_branch` remains clean. The subsequent enqueue of `t_a` will pass `assert_list_leaf_cfs_rq()`. Call `kstep_pass("tmp_alone_branch properly reset")`.

9. **Alternative approach — Using `sched_move_task` path:**
   If dynamically creating child cgroups is difficult in kSTEP, an alternative is to:
   - Create cgroups A, B, and C upfront (before throttling).
   - Trigger throttle on B's hierarchy.
   - Move a task into cgroup C using `kstep_cgroup_add_task("C", t_c->pid)` while B is throttled. This calls `sched_move_task()` → `attach_task_cfs_rq()` → `attach_entity_cfs_rq()` → `propagate_entity_cfs_rq()`, which hits the same code path.
   This approach may be simpler as it avoids dynamic cgroup creation during runtime.

10. **Expected kernel version guard:**
    ```c
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(6,18,0) || \
        (LINUX_VERSION_CODE >= KERNEL_VERSION(6,17,0) && LINUX_VERSION_CODE < KERNEL_VERSION(6,18,0))
    ```
    The bug was introduced in the v6.18 merge window (commits in linux-next/sched-core that appeared around v6.17-rc4 to v6.18-rc1). The guard should target v6.18-rc1 and later, before v6.18-rc3.

### Summary of kSTEP Changes Needed:

- **CFS bandwidth control:** A helper like `kstep_cgroup_set_bandwidth(name, quota_us, period_us)` to write `cpu.max`. This is a minor addition — just writing to the cgroup `cpu.max` file or calling `tg_set_cfs_quota()` / `tg_set_cfs_period()` internally.
- **Hierarchical cgroup creation:** If not already supported, support creating nested cgroups like `kstep_cgroup_create("B/C")` or `kstep_cgroup_create_child("B", "C")`. Alternatively, use the `sched_move_task` approach (moving a task into cgroup C while B is throttled) to avoid this need entirely.
- Both are minor, additive changes that fit naturally into kSTEP's existing cgroup management APIs.
