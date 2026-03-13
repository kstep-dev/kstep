# Topology: NULL pointer dereference in sched_numa_find_nth_cpu() when bsearch fails

**Commit:** `5ebf512f335053a42482ebff91e46c6dc156bf8c`
**Affected files:** kernel/sched/topology.c
**Fixed in:** v6.17-rc5
**Buggy since:** v6.3-rc1 (introduced by `cd7f55359c90 ("sched: add sched_numa_find_nth_cpu()")`)

## Bug Description

The function `sched_numa_find_nth_cpu()` in `kernel/sched/topology.c` finds the Nth closest CPU from a given cpumask, ordered by NUMA distance from a specified node. It uses a binary search (`bsearch()`) over `sched_domains_numa_masks` — a 2D array indexed by NUMA hop level and node — to determine the correct NUMA hop level that contains the requested CPU index. However, the code unconditionally dereferences the return value of `bsearch()` without checking for NULL.

The `bsearch()` function returns NULL when the comparison function (`hop_cmp()`) never returns 0 for any element in the array. This happens when the intersection of the caller-provided cpumask and the NUMA masks is insufficient at every NUMA level. Specifically, `hop_cmp()` returns 1 (search right) when `cpumask_weight_and(k->cpus, cur_hop[k->node]) <= k->cpu` — meaning the number of matching CPUs at the current hop level is less than or equal to the requested index. If this condition holds for all NUMA levels (including the highest, which normally covers all CPUs), `bsearch()` exhausts the array without finding a match and returns NULL.

The original real-world trigger was booting an ARM rk3399 (4 LITTLE + 2 big cores) with `maxcpus=4`, which left all big CPUs offline. When a driver called `smp_call_function_any()` with a cpumask containing only offline CPUs, it internally called `sched_numa_find_nth_cpu()`, which crashed due to the NULL dereference. However, the bug can also be triggered on any NUMA-capable system by passing a cpu index equal to or greater than the number of matching CPUs in the cpumask, since this causes the same `bsearch()` failure.

## Root Cause

The root cause is a missing NULL check on the return value of `bsearch()` in `sched_numa_find_nth_cpu()`. The relevant buggy code path is:

```c
int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node)
{
    struct __cmp_key k = { .cpus = cpus, .cpu = cpu };
    struct cpumask ***hop_masks;
    int hop, ret = nr_cpu_ids;

    ...
    k.masks = rcu_dereference(sched_domains_numa_masks);
    if (!k.masks)
        goto unlock;

    hop_masks = bsearch(&k, k.masks, sched_domains_numa_levels,
                        sizeof(k.masks[0]), hop_cmp);
    /* BUG: hop_masks can be NULL here */
    hop = hop_masks - k.masks;  /* NULL pointer arithmetic → crash */
    ...
}
```

The `hop_cmp()` comparison function operates as follows:

```c
static int hop_cmp(const void *a, const void *b)
{
    struct cpumask **cur_hop = *(struct cpumask ***)b;
    struct __cmp_key *k = (struct __cmp_key *)a;

    if (cpumask_weight_and(k->cpus, cur_hop[k->node]) <= k->cpu)
        return 1;  /* not enough CPUs at this level, search right */

    if (b == k->masks) {
        k->w = 0;
        return 0;  /* found: first level has enough */
    }

    prev_hop = *((struct cpumask ***)b - 1);
    k->w = cpumask_weight_and(k->cpus, prev_hop[k->node]);
    if (k->w <= k->cpu)
        return 0;  /* found: this level has enough, previous didn't */

    return -1;  /* previous level already had enough, search left */
}
```

The `bsearch()` function performs a standard binary search, returning the element for which `hop_cmp()` returns 0, or NULL if no such element exists. The function returns NULL when `hop_cmp()` returns 1 for all elements — meaning that at every NUMA level, the weighted intersection `cpumask_weight_and(k->cpus, masks[level][node])` is less than or equal to `k->cpu`.

This NULL return can happen in two scenarios:

1. **All CPUs in `cpus` are offline**: Since `sched_domains_numa_masks` only tracks online CPUs (via `sched_domains_numa_masks_set()` and `sched_domains_numa_masks_clear()`), a cpumask containing only offline CPUs will have zero intersection with all NUMA masks. With `cpu == 0`, the condition `0 <= 0` is true, so `hop_cmp` returns 1 for every level.

2. **cpu index exceeds available CPUs**: If `cpu >= cpumask_weight_and(cpus, highest_level_mask)`, then even the broadest NUMA level doesn't contain enough matching CPUs, and `hop_cmp` returns 1 for all levels.

After `bsearch()` returns NULL, the code computes `hop = hop_masks - k.masks` which is pointer arithmetic on NULL, yielding a bogus value. This bogus `hop` is then used to index into `k.masks[hop][node]`, causing a page fault when accessing an unmapped address.

## Consequence

The consequence is a kernel panic due to a NULL pointer dereference (or more precisely, an invalid memory access from pointer arithmetic on NULL). On ARM64, this manifests as:

```
Unable to handle kernel paging request at virtual address ffffff8000000000
Internal error: Oops: 0000000096000006 [#1] SMP
pc : sched_numa_find_nth_cpu+0x2a0/0x488
```

The crash occurs during early boot on the original reporter's system (an rk3399 with `maxcpus=4`), during PMU driver initialization:

```
sched_numa_find_nth_cpu ← smp_call_function_any ← armv8_pmu_init
    ← arm_pmu_device_probe ← platform_probe ← driver_attach
    ← bus_add_driver ← do_one_initcall ← kernel_init_freeable
```

Since the crash happens in PID 1 (`swapper/0` / `kernel_init`), the kernel panics with "Attempted to kill init!" and the system halts completely. No recovery is possible.

Beyond the early-boot scenario, any kernel code calling `sched_numa_find_nth_cpu()` with a cpumask that doesn't intersect with NUMA masks can trigger the same crash. The function is exported (`EXPORT_SYMBOL_GPL`) and called from `smp_call_function_any()`, which is a commonly used kernel API. Any driver or subsystem that calls `smp_call_function_any()` with a cpumask containing only offline CPUs would trigger this bug. This makes it a latent crash hazard for any system with NUMA topology and CPU hotplug activity.

## Fix Summary

The fix adds a single NULL check after the `bsearch()` call:

```c
hop_masks = bsearch(&k, k.masks, sched_domains_numa_levels,
                    sizeof(k.masks[0]), hop_cmp);
+   if (!hop_masks)
+       goto unlock;
    hop = hop_masks - k.masks;
```

When `bsearch()` returns NULL (indicating no NUMA level has enough matching CPUs), the function now jumps to the `unlock` label, which releases the RCU read lock and returns the default value `ret = nr_cpu_ids`. This signals "no CPU found" to the caller.

As noted in the commit message by Yury Norov, with this fix the call chain `smp_call_function_any() → smp_call_function_single() → generic_exec_single()` properly handles the `nr_cpu_ids` return by testing `cpu >= nr_cpu_ids` and returning `-ENXIO`. This is the expected error path for "no suitable CPU available" and is already handled throughout the kernel.

The fix is minimal, correct, and complete. It preserves the existing semantics (return `nr_cpu_ids` when no CPU is found) and does not change any other code paths. The fix was also tagged for stable backports (`Cc: stable@vger.kernel.org`), indicating it affects all kernels since v6.3-rc1 where `sched_numa_find_nth_cpu()` was introduced.

## Triggering Conditions

The bug requires the following precise conditions:

1. **CONFIG_NUMA=y**: The kernel must be compiled with NUMA support. Without this, `sched_numa_find_nth_cpu()` is an inline function that simply calls `cpumask_nth_and()`, which has no bsearch and no NULL dereference risk.

2. **sched_domains_numa_masks initialized**: The kernel must have completed `sched_init_numa()` during boot, which initializes the `sched_domains_numa_masks` array. This happens on any system with at least one NUMA node (including single-node systems). The existing code already handles the case where `sched_domains_numa_masks` is NULL (the `if (!k.masks) goto unlock;` check), so the bug specifically requires the masks to exist but the bsearch to fail.

3. **bsearch() returns NULL**: This requires that `hop_cmp()` returns 1 for ALL NUMA levels. This happens when:
   - **Scenario A (offline CPUs)**: The `cpus` argument contains only offline CPUs. Since NUMA masks track only online CPUs, the intersection is empty at all levels. With `cpu == 0`, the condition `0 <= 0` holds for all levels.
   - **Scenario B (out-of-range index)**: The `cpu` argument (the index of the CPU to find, 0-based) is greater than or equal to the number of CPUs in the intersection of `cpus` and the broadest NUMA mask. For example, on a 2-CPU system, calling with `cpu == 2` and `cpus = cpu_online_mask` causes `cpumask_weight_and` to return 2, and `2 <= 2` is true, so `hop_cmp` returns 1.

4. **NUMA node parameter**: The `node` parameter must not be `NUMA_NO_NODE` (-1), because that triggers an early return via `cpumask_nth_and()` without reaching the bsearch code.

The simplest and most reliable trigger for kSTEP is Scenario B: call `sched_numa_find_nth_cpu(cpu_online_mask, num_online_cpus(), 0)` on any NUMA-capable system. This requests the Nth closest CPU where N equals the total number of online CPUs — a valid-looking but out-of-bounds index that causes bsearch to fail. Since kSTEP already compiles kernels with `CONFIG_NUMA=y`, and QEMU initializes at least one NUMA node, this trigger works without any framework changes.

## Reproduce Strategy (kSTEP)

The reproduction strategy is straightforward because the bug can be triggered by a single function call with specific parameters, without requiring CPU hotplug, complex topology, or workload setup.

### Step 1: Kernel Version and Configuration

Use the buggy kernel at commit `5ebf512f335053a42482ebff91e46c6dc156bf8c~1` (one before the fix). The kSTEP kernel config already includes `CONFIG_NUMA=y`, which is the only configuration requirement. No other kernel config changes are needed. Build with at least 2 CPUs (the default).

```
./checkout_linux.py 5ebf512f335053a42482ebff91e46c6dc156bf8c~1 numa_find_nth_buggy
make linux LINUX_NAME=numa_find_nth_buggy
```

### Step 2: Driver Setup

Create a minimal driver that calls `sched_numa_find_nth_cpu()`. The function is `EXPORT_SYMBOL_GPL`, so it can be called directly from a kernel module without `KSYM_IMPORT`. The driver needs:

- No tasks (no `kstep_task_create()`)
- No topology setup (default NUMA topology is sufficient)
- No cgroup configuration
- No tick advancement
- Only the `run` callback

### Step 3: Trigger the Bug

In the `run()` function, call `sched_numa_find_nth_cpu()` with these parameters:

```c
#include <linux/topology.h>

int ret = sched_numa_find_nth_cpu(cpu_online_mask, num_online_cpus(), 0);
```

- `cpus = cpu_online_mask`: A valid cpumask containing all online CPUs.
- `cpu = num_online_cpus()`: The 0-based index equal to the count of online CPUs. Valid indices are `0` to `num_online_cpus()-1`, so this is one past the last valid index.
- `node = 0`: A valid NUMA node (node 0 always exists).

This causes `hop_cmp()` to return 1 for all NUMA levels because at every level, `cpumask_weight_and(cpu_online_mask, masks[level][0])` equals `num_online_cpus()`, and `num_online_cpus() <= num_online_cpus()` is true. The `bsearch()` returns NULL, and the subsequent `hop = hop_masks - k.masks` dereferences NULL.

### Step 4: Detection

On the **buggy kernel**, the call will trigger a kernel oops/panic due to NULL pointer dereference. The driver should detect this by checking if execution continues past the call:

```c
static void run(void) {
    int ret;

    pr_info("Calling sched_numa_find_nth_cpu with out-of-range index...\n");
    ret = sched_numa_find_nth_cpu(cpu_online_mask, num_online_cpus(), 0);

    /* If we reach here, the bug is fixed */
    if (ret == nr_cpu_ids) {
        kstep_pass("sched_numa_find_nth_cpu returned nr_cpu_ids (%d) for out-of-range index", ret);
    } else {
        kstep_fail("sched_numa_find_nth_cpu returned unexpected value %d", ret);
    }
}
```

On the **buggy kernel**: The kernel will crash before reaching the pass/fail check. The crash will appear in the logs as a NULL pointer dereference in `sched_numa_find_nth_cpu`. The driver output file will have no pass/fail entry.

On the **fixed kernel**: The function safely returns `nr_cpu_ids`, execution continues, and the driver reports pass.

### Step 5: Alternative Trigger (Offline CPUs)

An alternative trigger closer to the original bug report would use offline CPUs. This requires adding a `kstep_cpu_offline(cpu)` helper to kSTEP (a minor extension wrapping the kernel's `cpu_down()` API). The sequence would be:

1. Take CPU 1 offline: `kstep_cpu_offline(1)`
2. Create a cpumask containing only CPU 1: `cpumask_set_cpu(1, &mask)`
3. Call `sched_numa_find_nth_cpu(&mask, 0, 0)` — requesting the 0th closest CPU from a mask of entirely offline CPUs

This more closely mimics the original bug scenario but is unnecessary for basic reproduction since the out-of-range index trigger exercises the exact same NULL pointer dereference code path.

### Step 6: Run and Verify

```bash
# Buggy kernel - expect crash (no pass/fail in output)
./run.py topology_numa_find_nth_null --linux_name numa_find_nth_buggy
cat data/logs/latest.log  # Should show NULL pointer dereference

# Fixed kernel
./checkout_linux.py 5ebf512f335053a42482ebff91e46c6dc156bf8c numa_find_nth_fixed
make linux LINUX_NAME=numa_find_nth_fixed
./run.py topology_numa_find_nth_null --linux_name numa_find_nth_fixed
cat data/logs/latest.log  # Should show pass
```

### Step 7: Version Guard

Guard the driver with `#if LINUX_VERSION_CODE` to only run on kernels where the function exists (v6.3+, since `sched_numa_find_nth_cpu()` was introduced in `cd7f55359c90`):

```c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
```

### Expected Behavior Summary

| Kernel | Behavior |
|--------|----------|
| Buggy (pre-fix) | Kernel oops: NULL pointer dereference in `sched_numa_find_nth_cpu+0x2a0` |
| Fixed (post-fix) | Function returns `nr_cpu_ids`, driver reports pass |
