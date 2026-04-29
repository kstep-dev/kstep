# Topology: sched_relax_domain_level Off-by-One Prevents Disabling Newidle Balancing

**Commit:** `a1fd0b9d751f840df23ef0e75b691fc00cfd4743`
**Affected files:** `kernel/sched/topology.c`, `kernel/cgroup/cpuset.c`
**Fixed in:** v6.10-rc1
**Buggy since:** v2.6.26-rc1 (commit `1d3504fcf560` "sched, cpuset: customize sched domains, core")

## Bug Description

The `cpuset.sched_relax_domain_level` cgroup parameter is intended to control the scope of newidle load balancing for tasks within a cpuset. According to the kernel documentation, setting this value to `0` means "no search" — i.e., newidle balancing should be completely disabled across all scheduling domain levels. Setting it to `1` means "search siblings (hyperthreads in a core)" only, `2` means "search cores in a package", and so on. Each value N is supposed to enable newidle balancing on domains with levels 0 through N-1 and disable it at level N and above.

However, due to two off-by-one errors — one in `set_domain_attribute()` in `kernel/sched/topology.c` and one in `update_relax_domain_level()` in `kernel/cgroup/cpuset.c` — setting `sched_relax_domain_level` to `0` does not actually disable newidle balancing at any level. Instead, it behaves identically to setting the value to `1`, leaving level-0 (SMT/sibling) domain's `SD_BALANCE_NEWIDLE` flag intact. The "no search" option documented for value `0` is effectively unreachable.

Additionally, the upper-bound validation in `update_relax_domain_level()` uses `val >= sched_domain_level_max` which prevents setting the level high enough to enable newidle balancing on ALL domain levels. On a system with `sched_domain_level_max = N`, the maximum valid value should be `N+1` (to include all N levels), but the old code only allows up to `N-1`.

This bug was discovered in a real-world production scenario during a Linux upgrade from 5.4 to 6.x. A video encoding workload with hundreds to thousands of threads using futexes and nanosleep syscalls experienced a ~3% performance regression. The regression was traced to newidle balancing overhead: `sched_balance_newidle()` was consuming significant CPU time in `load_balance()` → `find_busiest_group()` → `update_sd_lb_stats()`. The user attempted to disable newidle balancing via `sched_relax_domain_level=0` but found it did not work as documented.

## Root Cause

There are two related off-by-one errors:

**Error 1: `set_domain_attribute()` in `kernel/sched/topology.c`**

The function `set_domain_attribute()` iterates over all scheduling domains and is responsible for clearing the `SD_BALANCE_WAKE` and `SD_BALANCE_NEWIDLE` flags on domains that should not participate in idle-time balancing based on the `relax_domain_level` setting. The buggy code reads:

```c
if (sd->level > request) {
    /* Turn off idle balance on this domain: */
    sd->flags &= ~(SD_BALANCE_WAKE|SD_BALANCE_NEWIDLE);
}
```

The comparison `sd->level > request` means that when `request = 0`, only domains with `sd->level > 0` (i.e., level 1 and above) have their flags cleared. The level-0 domain (SMT siblings) retains `SD_BALANCE_NEWIDLE` because `0 > 0` is false. The correct semantics require `sd->level >= request`, so that `request = 0` disables newidle balancing at level 0 and all higher levels.

The intended semantics are: the `request` value specifies the number of domain levels that should have newidle balancing enabled. A request of `0` means zero levels enabled (no search). A request of `1` means only level-0 enabled (search siblings). A request of `2` means levels 0 and 1 enabled, and so forth. Under these semantics, the condition to DISABLE newidle balancing on a domain is `sd->level >= request`, not `sd->level > request`.

**Error 2: `update_relax_domain_level()` in `kernel/cgroup/cpuset.c`**

The validation of the `sched_relax_domain_level` value uses:

```c
if (val < -1 || val >= sched_domain_level_max)
    return -EINVAL;
```

The variable `sched_domain_level_max` represents the maximum domain level in the system. On a system with three domain levels (0=SMT, 1=MC, 2=NUMA), `sched_domain_level_max` would be approximately 3 (one past the highest level). The condition `val >= sched_domain_level_max` prevents users from setting a value that would enable newidle balancing on ALL domain levels. To include all levels, the user needs to set the value to `sched_domain_level_max + 1`, but the validation rejects that. The fix changes the check to `val > sched_domain_level_max + 1`, allowing values from -1 up to `sched_domain_level_max + 1` inclusive.

## Consequence

The observable impact is a performance degradation for workloads that intend to disable newidle balancing. Specifically:

- **Inability to disable newidle balancing at the lowest domain level.** When a user sets `cpuset.sched_relax_domain_level = 0` (documented as "no search"), newidle balancing is still performed at the SMT/sibling domain level. This results in unnecessary calls to `sched_balance_newidle()` → `load_balance()` on every CPU going idle, incurring overhead from `find_busiest_group()` and `update_sd_lb_stats()`. Perf profiling from the original reporter showed `newidle_balance` consuming 1.87% and 1.12% of CPU time in the `futex_wait` and `nanosleep` paths respectively, which disappeared when newidle balancing was properly disabled.

- **Inability to enable newidle balancing at all levels.** The overly restrictive upper-bound validation prevents users from setting `sched_relax_domain_level` high enough to explicitly enable newidle balancing on the highest-level domain (e.g., NUMA). Users who want to override a platform default that excludes the top-level domain cannot do so.

- **The ~3% performance regression** reported by the patch author was on a dual-socket Intel Xeon E5-2680 v3 server running a video encoding workload with hundreds to thousands of threads synchronized via mutexes and condition variables. The workload intentionally keeps some CPUs idle, making newidle balancing particularly expensive due to frequent idle transitions triggering load balancing scans.

This is not a crash or data corruption bug — it is a functional/performance bug where a documented user-facing interface does not behave as specified.

## Fix Summary

The fix applies two minimal changes:

1. **In `set_domain_attribute()` (topology.c):** Change `if (sd->level > request)` to `if (sd->level >= request)`. This ensures that when the user requests level 0, the level-0 domain (and all higher domains) will have `SD_BALANCE_WAKE` and `SD_BALANCE_NEWIDLE` cleared. The semantics become: the `request` value is the number of bottom-up levels to KEEP newidle-enabled. Level 0 means keep zero levels (disable all), level 1 means keep one level (level 0 only), etc.

2. **In `update_relax_domain_level()` (cpuset.c):** Change `if (val < -1 || val >= sched_domain_level_max)` to `if (val < -1 || val > sched_domain_level_max + 1)`. This extends the valid range so that users can set `sched_relax_domain_level` up to `sched_domain_level_max + 1`, which enables newidle balancing on ALL domain levels including the highest. The value `sched_domain_level_max` enables all levels except the highest, and `sched_domain_level_max + 1` includes the highest too.

The fix is correct and complete because it aligns the runtime behavior with the documented interface semantics: value 0 means "no search", value 1 means "search siblings", and higher values progressively include more domain levels up to the system-wide level. The fix was reviewed by Vincent Guittot and Valentin Schneider, and tested by Dietmar Eggemann.

## Triggering Conditions

The bug triggers whenever a user or system administrator sets `cpuset.sched_relax_domain_level` to `0` expecting to completely disable newidle load balancing:

- **Required topology:** The system must have at least two scheduling domain levels. Typically this means SMT-capable CPUs (level 0 = SMT, level 1 = MC), or a multi-socket/NUMA system. On a system with only one domain level, the bug would be less visible since there's only one level to disable.

- **Cgroup configuration:** A cpuset cgroup must be created with `sched_load_balance` enabled (the default) and `sched_relax_domain_level` set to `0`. This can be done via cgroup v1 (`echo 0 > /sys/fs/cgroup/cpuset/<name>/cpuset.sched_relax_domain_level`) or via the boot parameter `relax_domain_level=0` for the system-wide default.

- **Workload:** The impact is most visible with workloads that have frequent idle transitions (tasks blocking on futexes, mutexes, nanosleep, etc.) because each idle transition triggers `sched_balance_newidle()`. Workloads with hundreds or thousands of threads that don't keep all CPUs 100% busy are particularly affected.

- **No race condition or timing requirement:** This is a deterministic logic bug in the comparison operator. Every single invocation of `set_domain_attribute()` with `request = 0` will incorrectly leave level-0 newidle balancing enabled. The bug is 100% reproducible.

- **Kernel configuration:** `CONFIG_SMP` must be enabled (virtually all production kernels). `CONFIG_CPUSETS` must be enabled for the cgroup interface. No other special configuration is needed.

## Reproduce Strategy (kSTEP)

The reproduction strategy leverages kSTEP's topology, cgroup, and scheduler domain introspection capabilities. The key idea is to set up a multi-level topology, write `sched_relax_domain_level = 0` to a cpuset, and then check whether `SD_BALANCE_NEWIDLE` is still set on the level-0 domain.

### Required kSTEP Extension

kSTEP currently does not have a `kstep_cgroup_set_relax_domain_level()` function. However, this is a **minor extension** similar to the existing `kstep_cgroup_set_weight()`. It would involve writing to the cpuset's `sched_relax_domain_level` cgroup file. The implementation pattern is identical to `kstep_cgroup_set_weight()` — open the cgroup attribute file, write the integer value, and close it. Alternatively, since `relax_domain_level` can be set via the kernel boot parameter `relax_domain_level=`, kSTEP could pass this parameter to QEMU, which would not require any kSTEP code changes.

A second approach that avoids extending kSTEP: use `KSYM_IMPORT()` to import `default_relax_domain_level` and write to it directly, then trigger a sched domain rebuild. However, this is more fragile.

### Step-by-Step Driver Plan

1. **Topology setup:** Configure at least 2 CPUs with SMT topology so that two domain levels exist (level 0 = SMT, level 1 = MC or higher):
   ```c
   kstep_topo_init();
   kstep_topo_set_smt(0, 1);  // CPUs 0 and 1 are SMT siblings
   kstep_topo_apply();
   ```
   This creates a level-0 (SMT) domain spanning CPUs 0-1 and a level-1 domain. Alternatively, use 4 CPUs with `kstep_topo_set_smt(0,1)` and `kstep_topo_set_smt(2,3)` plus `kstep_topo_set_mc(0,1,2,3)` to create three levels.

2. **Import necessary symbols:**
   ```c
   KSYM_IMPORT(sched_domain_level_max);
   ```
   Also, use `cpu_rq()` and per-CPU sched_domain access from `internal.h`.

3. **Set relax_domain_level to 0:** Either:
   - (a) Use the proposed `kstep_cgroup_set_relax_domain_level("", 0)` extension, or
   - (b) Import and directly set `default_relax_domain_level`:
     ```c
     KSYM_IMPORT(default_relax_domain_level);
     *default_relax_domain_level = 0;
     ```
     Then trigger a sched domain rebuild by writing the cpuset's `sched_load_balance` (toggle it off and on), or by calling `rebuild_sched_domains()` directly if accessible.

4. **Create tasks and trigger newidle balancing:**
   ```c
   struct task_struct *t1 = kstep_task_create();
   kstep_task_pin(t1, 1, 2);  // pin to CPU 1
   kstep_task_wakeup(t1);
   kstep_tick_repeat(10);     // let things settle
   kstep_task_block(t1);      // block task so CPU 1 goes idle
   ```
   When CPU 1 goes idle, the scheduler will call `sched_balance_newidle()`.

5. **Check SD_BALANCE_NEWIDLE flag on level-0 domain:**
   ```c
   struct rq *rq = cpu_rq(1);
   struct sched_domain *sd;
   rcu_read_lock();
   sd = rcu_dereference(rq->sd);  // lowest-level domain
   if (sd && sd->level == 0) {
       if (sd->flags & SD_BALANCE_NEWIDLE) {
           kstep_fail("BUG: SD_BALANCE_NEWIDLE still set on level-0 domain with relax_domain_level=0");
       } else {
           kstep_pass("SD_BALANCE_NEWIDLE correctly cleared on level-0 domain");
       }
   }
   rcu_read_unlock();
   ```

6. **Use `on_sched_balance_begin` callback for behavioral detection:**
   As an alternative or additional check, use the `on_sched_balance_begin` callback to observe whether newidle balancing is attempted on the level-0 domain:
   ```c
   static bool newidle_on_level0 = false;
   void on_sched_balance_begin(int cpu, struct sched_domain *sd) {
       if (sd->level == 0) {
           newidle_on_level0 = true;
       }
   }
   ```
   After triggering idle transitions, check `newidle_on_level0`. On the buggy kernel it will be true; on the fixed kernel it will be false (because SD_BALANCE_NEWIDLE is cleared, so `sched_balance_rq` won't be called for newidle at that level).

7. **Expected results:**
   - **Buggy kernel (before fix):** `SD_BALANCE_NEWIDLE` is still set on the level-0 domain when `relax_domain_level = 0`. Newidle balancing occurs on level-0. The `on_sched_balance_begin` callback fires for level-0. The driver reports FAIL.
   - **Fixed kernel (after fix):** `SD_BALANCE_NEWIDLE` is cleared on the level-0 domain (and all domains) when `relax_domain_level = 0`. No newidle balancing at any level. The driver reports PASS.

### Validation approach

The most robust detection method is directly reading `sd->flags` on each domain level after setting `relax_domain_level`. Walk the sched_domain chain from `rq->sd` upward via `sd->parent` and verify that no domain has `SD_BALANCE_NEWIDLE` set:

```c
rcu_read_lock();
for_each_domain(cpu, sd) {
    if (sd->flags & SD_BALANCE_NEWIDLE) {
        kstep_fail("level %d still has SD_BALANCE_NEWIDLE", sd->level);
        break;
    }
}
rcu_read_unlock();
```

This flag-based check is fully deterministic and does not depend on timing or workload behavior.

### QEMU configuration

- At least 2 CPUs (preferably 4 to create a richer topology)
- Standard RAM (128MB or more)
- No special hardware requirements
