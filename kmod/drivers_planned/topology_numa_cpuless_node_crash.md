# Topology: NULL dereference in sched_numa_find_nth_cpu() for CPU-less NUMA node

**Commit:** `617f2c38cb5ce60226042081c09e2ee3a90d03f8`
**Affected files:** kernel/sched/topology.c
**Fixed in:** v6.7-rc1
**Buggy since:** v6.3-rc1 (introduced by `cd7f55359c90 ("sched: add sched_numa_find_nth_cpu()")`)

## Bug Description

The function `sched_numa_find_nth_cpu()` in `kernel/sched/topology.c` finds the Nth closest CPU from a given cpumask, ordered by NUMA distance from a specified starting node. It relies on the `sched_domains_numa_masks` data structure — a 3D array indexed by `[hop_level][node]` — where each entry is a cpumask representing all CPUs reachable from `node` within `hop_level` NUMA hops.

During boot, `sched_init_numa()` initializes `sched_domains_numa_masks` but **only for nodes that have CPUs** (i.e., nodes with `node_state(node, N_CPU) == true`). Entries for CPU-less NUMA nodes are left uninitialized (NULL). When `sched_numa_find_nth_cpu()` is called with the ID of a CPU-less node, it passes this node index into the `hop_cmp()` comparator function, which accesses `cur_hop[k->node]`. Since `cur_hop[node]` is NULL for a CPU-less node, dereferencing it in `cpumask_weight_and()` triggers a NULL pointer dereference, crashing the kernel.

This bug was reported independently by two people: Yicong Yang from Huawei encountered it on a TaiShan 2280 V2 ARM server when booting with `maxcpus=1`, which left some NUMA nodes without any online CPUs. Guenter Roeck encountered a crash on sparc64 QEMU images during testing of the `for_each_numa_cpus()` test series. Both crashes stem from the same root cause: the code did not account for CPU-less NUMA nodes when indexing into `sched_domains_numa_masks`.

The function `sched_numa_find_nth_cpu()` is exported via `EXPORT_SYMBOL_GPL` and is called from `cpumask_local_spread()`, which is widely used by device drivers (e.g., networking drivers like hns3) to spread interrupt vectors across CPUs near a device's NUMA node. If the device is attached to a CPU-less NUMA node, the driver's call to `cpumask_local_spread()` triggers the crash during device initialization.

## Root Cause

The root cause lies in the asymmetry between how `sched_domains_numa_masks` is populated and how it is accessed:

**Population (during boot in `sched_init_numa()`):** The array `sched_domains_numa_masks[level][node]` is allocated and filled only for nodes that have CPUs. Specifically, the initialization loop in `sched_init_numa()` iterates over `for_each_node_state(i, N_CPU)` when setting up the masks. Nodes that do not have any CPUs (e.g., memory-only NUMA nodes, or nodes whose CPUs are offlined via `maxcpus=` boot parameter) are skipped entirely. Their corresponding entries in `sched_domains_numa_masks[level][node]` remain NULL.

**Access (in `sched_numa_find_nth_cpu()`):** The function accepts any `node` parameter and directly uses it to index into the masks without checking whether that node has CPUs. The buggy code path is:

```c
int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node)
{
    struct __cmp_key k = { .cpus = cpus, .node = node, .cpu = cpu };
    // ...
    k.masks = rcu_dereference(sched_domains_numa_masks);
    if (!k.masks)
        goto unlock;

    hop_masks = bsearch(&k, k.masks, sched_domains_numa_levels, sizeof(k.masks[0]), hop_cmp);
    // ...
}
```

The `bsearch()` calls `hop_cmp()`, which accesses:
```c
static int hop_cmp(const void *a, const void *b)
{
    struct cpumask **cur_hop = *(struct cpumask ***)b;
    struct __cmp_key *k = (struct __cmp_key *)a;

    if (cpumask_weight_and(k->cpus, cur_hop[k->node]) <= k->cpu)
        return 1;
    // ...
}
```

When `k->node` refers to a CPU-less node, `cur_hop[k->node]` is NULL. The call to `cpumask_weight_and(k->cpus, NULL)` then dereferences the NULL pointer inside `__bitmap_weight_and()`, causing an immediate kernel oops.

The critical issue is that there is no validation of the `node` parameter before it is used. The code assumes that any valid NUMA node ID passed to the function will have corresponding entries in `sched_domains_numa_masks`, but this assumption fails for CPU-less nodes.

## Consequence

The immediate consequence is a **kernel panic via NULL pointer dereference**. The crash occurs in `__bitmap_weight_and()` when it tries to read bits from a NULL cpumask pointer. On ARM64 (the platform where it was first reported), this manifests as:

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Internal error: Oops: 0000000096000004 [#1] PREEMPT SMP
pc : __bitmap_weight_and+0x40/0xb0
lr : cpumask_weight_and+0x18/0x24
Call trace:
 __bitmap_weight_and+0x40/0xb0
 cpumask_weight_and+0x18/0x24
 hop_cmp+0x2c/0xa4
 bsearch+0x50/0xc0
 sched_numa_find_nth_cpu+0x80/0x130
 cpumask_local_spread+0x38/0xa8
 hns3_nic_init_vector_data+0x58/0x394
 ...
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
```

In the reported case, the crash occurred during early boot (PID 1, `kernel_init_freeable()`) when a network driver (hns3) called `cpumask_local_spread()` to distribute interrupt vectors. Since this happened during `do_one_initcall()` processing, the crash killed the init process, resulting in a complete system failure with no possibility of recovery.

This bug affects any system with CPU-less NUMA nodes. This is a realistic configuration in several scenarios: (1) booting with `maxcpus=N` where N is less than the total CPU count, leaving some NUMA nodes without active CPUs; (2) memory-only NUMA nodes (common in CXL and HBM configurations); (3) CPU hotplug scenarios where all CPUs on a node are taken offline. Any driver or kernel subsystem that calls `cpumask_local_spread()`, `sched_numa_find_nth_cpu()`, or `for_each_numa_cpu()` with a CPU-less node ID will trigger this crash.

## Fix Summary

The fix adds a single check at the beginning of `sched_numa_find_nth_cpu()` to handle CPU-less nodes gracefully. Before accessing `sched_domains_numa_masks`, the function now maps the input `node` to the nearest node that actually has CPUs using `numa_nearest_node(node, N_CPU)`:

```c
int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node)
{
    struct __cmp_key k = { .cpus = cpus, .cpu = cpu };  // node NOT set here
    // ...
    rcu_read_lock();

    /* CPU-less node entries are uninitialized in sched_domains_numa_masks */
    node = numa_nearest_node(node, N_CPU);
    k.node = node;

    k.masks = rcu_dereference(sched_domains_numa_masks);
    // ...
}
```

The key changes are: (1) The `k.node` field is no longer initialized in the struct initializer along with `cpus` and `cpu`. Instead, it is set after the `numa_nearest_node()` call. (2) The `numa_nearest_node()` function finds the nearest NUMA node (by `node_distance()`) that has the `N_CPU` state set, i.e., the nearest node that actually has CPUs. This ensures that the node used to index into `sched_domains_numa_masks` always has valid (non-NULL) entries.

This fix is correct because the semantics of `sched_numa_find_nth_cpu()` are to find the Nth closest CPU from a given cpumask ordered by NUMA distance. If the starting node has no CPUs, the nearest node with CPUs is the best approximation — it will produce results with minimal NUMA distance deviation. The behavior is consistent with what a user would expect: find CPUs close to the specified location, falling back to the nearest populated node if the exact node has no CPUs.

The fix also implicitly handles Yicong Yang's initial concern about `sched_numa_hop_mask()`, since that function is a separate code path. However, the commit specifically addresses only `sched_numa_find_nth_cpu()`. The `sched_numa_hop_mask()` fix was handled in the same patch series but potentially in a separate commit.

## Triggering Conditions

1. **CONFIG_NUMA=y**: The kernel must be compiled with NUMA support. Without this, `sched_numa_find_nth_cpu()` is a simple inline fallback that doesn't use `sched_domains_numa_masks`.

2. **At least two NUMA nodes, with at least one CPU-less**: The system must have multiple NUMA nodes, and at least one node must have no CPUs. This can happen through:
   - **Memory-only NUMA nodes**: Hardware configurations where a node has memory but no processor cores (e.g., CXL-attached memory, HBM nodes).
   - **Boot parameter `maxcpus=N`**: Limiting the number of CPUs at boot time can leave some NUMA nodes without any active CPUs.
   - **CPU hotplug**: All CPUs on a particular node could be offlined at runtime.

3. **A call to `sched_numa_find_nth_cpu()` with the CPU-less node ID**: This can happen through:
   - `cpumask_local_spread(idx, node)` where `node` is CPU-less — used by device drivers to spread interrupt vectors.
   - `for_each_numa_cpu()` with a CPU-less starting node.
   - Direct call to `sched_numa_find_nth_cpu()` from any kernel module (the function is `EXPORT_SYMBOL_GPL`).

4. **`sched_domains_numa_masks` is initialized**: The kernel must have completed `sched_init_numa()` during boot. The existing code handles the case where `sched_domains_numa_masks` is entirely NULL (the `if (!k.masks) goto unlock;` check). The bug requires the masks array to exist but have NULL entries for the CPU-less node.

The crash is **100% deterministic** — there is no race condition or timing dependency. Any single call to `sched_numa_find_nth_cpu()` with a CPU-less node ID will crash immediately on the first invocation of `hop_cmp()`.

## Reproduce Strategy (kSTEP)

This bug requires a NUMA topology with a CPU-less node, which is a hardware-level configuration set at QEMU boot time. Reproducing it in kSTEP requires a **minor extension** to the framework to support QEMU NUMA configuration.

### Required kSTEP Extension

The primary change needed is to allow kSTEP drivers to specify QEMU NUMA topology in the driver configuration. Currently, `run.py` invokes QEMU with `-smp N` but no `-numa` flags, creating a UMA (single-node) system. The extension would:

1. **Add a `numa_nodes` configuration option to the driver spec** (e.g., in the driver's Python metadata or a JSON config), allowing specification like:
   ```
   numa_nodes = [
       {"nodeid": 0, "cpus": "0-1", "mem": "128M"},
       {"nodeid": 1, "mem": "64M"}  # CPU-less node
   ]
   ```

2. **Modify `run.py`** (around line 111) to translate this into QEMU arguments:
   ```
   -object memory-backend-ram,id=mem0,size=128M
   -object memory-backend-ram,id=mem1,size=64M
   -numa node,nodeid=0,cpus=0-1,memdev=mem0
   -numa node,nodeid=1,memdev=mem1
   ```

3. **Ensure the kernel is compiled with `CONFIG_NUMA=y`** (check the kSTEP kernel config).

### Driver Design

Once the QEMU NUMA extension is in place, the driver is straightforward:

1. **Number of CPUs**: 2 (both assigned to NUMA node 0).
2. **NUMA topology**: 2 nodes — node 0 has CPUs 0-1, node 1 has memory only (CPU-less).
3. **Memory**: 256M total (128M per node).
4. **Task properties**: No tasks are needed. The driver itself calls the buggy function directly.

### Step-by-step reproduction

1. **Configure QEMU**: Launch with 2 CPUs on node 0 and a CPU-less node 1 (via the kSTEP NUMA extension).

2. **In the driver's `run()` function**:
   ```c
   #include <linux/topology.h>

   // sched_numa_find_nth_cpu is EXPORT_SYMBOL_GPL, callable from modules
   extern int sched_numa_find_nth_cpu(const struct cpumask *cpus, int cpu, int node);

   void run(void)
   {
       int ret;
       int node;

       // Find a CPU-less NUMA node
       for_each_node(node) {
           if (!node_state(node, N_CPU)) {
               pr_info("Found CPU-less node: %d\n", node);

               // This call crashes on the buggy kernel (NULL deref in hop_cmp)
               // On the fixed kernel, it returns a valid CPU from the nearest node
               ret = sched_numa_find_nth_cpu(cpu_online_mask, 0, node);

               if (ret < nr_cpu_ids) {
                   kstep_pass("sched_numa_find_nth_cpu returned CPU %d for CPU-less node %d", ret, node);
               } else {
                   kstep_pass("sched_numa_find_nth_cpu returned nr_cpu_ids for CPU-less node %d", node);
               }
               return;
           }
       }

       kstep_fail("No CPU-less NUMA node found — QEMU NUMA config may be wrong");
   }
   ```

3. **Detection criteria**:
   - **Buggy kernel**: The call to `sched_numa_find_nth_cpu()` triggers a NULL pointer dereference in `__bitmap_weight_and()` → `cpumask_weight_and()` → `hop_cmp()` → `bsearch()`. The kernel will oops and panic. The log will contain `Unable to handle kernel NULL pointer dereference` and a stack trace through `hop_cmp` and `sched_numa_find_nth_cpu`.
   - **Fixed kernel**: The `numa_nearest_node(node, N_CPU)` call remaps node 1 to node 0 (the nearest node with CPUs). The function returns a valid CPU (0 or 1) and `kstep_pass()` is called.

4. **No callbacks needed**: This is a simple direct function call test. No tick manipulation, task creation, or scheduling callbacks are required.

5. **Kernel version guard**: The driver should be guarded with `#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,3,0) && LINUX_VERSION_CODE < KERNEL_VERSION(6,7,0)` to only run on kernels where the bug exists (introduced in v6.3-rc1 by `cd7f55359c90`, fixed in v6.7-rc1 by this commit).

### Alternative approach without QEMU NUMA extension

If extending QEMU's NUMA configuration is not desired, there is a possible workaround: boot with the `maxcpus=1` kernel parameter on a system configured with 2 NUMA nodes, each having 1 CPU. This would leave node 1 CPU-less. However, this approach has downsides: (1) it requires kSTEP to support custom kernel boot parameters (another extension), (2) it reduces the available CPUs to 1, which may conflict with kSTEP's assumption that CPU 0 is reserved for the driver.

### Expected behavior summary

| Kernel  | Behavior |
|---------|----------|
| Buggy (v6.3 – v6.6.x) | NULL pointer dereference in `hop_cmp()` → kernel oops/panic |
| Fixed (v6.7+) | `numa_nearest_node()` remaps CPU-less node → returns valid CPU |
