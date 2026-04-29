# Topology: Incorrect Correction of Non-Topological SD_flags in sd_init()

**Commit:** `9b1b234bb86bcdcdb142e900d39b599185465dbb`
**Affected files:** kernel/sched/topology.c
**Fixed in:** v5.9-rc1
**Buggy since:** v3.16-rc1 (introduced by commit `143e1e28cb40` "sched: Rework sched_domain topology definition")

## Bug Description

During scheduler domain initialization, the kernel builds a hierarchy of `sched_domain` structures that represent the CPU topology (SMT, core, package, NUMA node, etc.). Each topology level can provide a set of SD_flags via a `tl->sd_flags()` callback. These flags control scheduling behaviors like capacity sharing, NUMA placement, and asymmetric packing. A subset of these flags are designated as "topology flags" (defined by the `TOPOLOGY_SD_FLAGS` macro), which are the only flags that topology levels are supposed to return.

The `sd_init()` function in `kernel/sched/topology.c` contains a safety check: if `tl->sd_flags()` returns any non-topological flags, a `WARN_ONCE` is fired and the offending flags are supposed to be stripped. However, due to a bitwise operator error, the correction logic does the exact opposite of what was intended — it strips the *topological* flags and keeps the *non-topological* flags.

Specifically, the buggy code reads `sd_flags &= ~TOPOLOGY_SD_FLAGS`, which masks *out* all topology flags (like `SD_SHARE_CPUCAPACITY`, `SD_SHARE_PKG_RESOURCES`, `SD_NUMA`, `SD_ASYM_PACKING`, `SD_SHARE_POWERDOMAIN`) and retains whatever non-topological flags were erroneously returned. The correct code should be `sd_flags &= TOPOLOGY_SD_FLAGS`, which keeps only the topology flags and discards any non-topological ones.

This bug has been latent since v3.16 when the sched_domain topology definition was reworked. It would only manifest if a topology level's `sd_flags()` callback actually returned a non-topological flag, which is uncommon in standard kernel configurations but could occur with architecture-specific or custom topology definitions.

## Root Cause

The root cause is a simple but critical bitwise logic error in the `sd_init()` function in `kernel/sched/topology.c`. The relevant code path is:

```c
if (tl->sd_flags)
    sd_flags = (*tl->sd_flags)();
if (WARN_ONCE(sd_flags & ~TOPOLOGY_SD_FLAGS,
        "wrong sd_flags in topology description\n"))
    sd_flags &= ~TOPOLOGY_SD_FLAGS;  /* BUG: should be sd_flags &= TOPOLOGY_SD_FLAGS */
```

The `TOPOLOGY_SD_FLAGS` macro defines the set of flags that are valid for topology levels to return:

```c
#define TOPOLOGY_SD_FLAGS       \
    (SD_SHARE_CPUCAPACITY   |   \
     SD_SHARE_PKG_RESOURCES |   \
     SD_NUMA                |   \
     SD_ASYM_PACKING        |   \
     SD_SHARE_POWERDOMAIN)
```

The condition `sd_flags & ~TOPOLOGY_SD_FLAGS` correctly checks whether any bits outside `TOPOLOGY_SD_FLAGS` are set in `sd_flags`. If true, it means the topology level returned non-topological flags.

The intent of the correction is to strip those non-topological bits while preserving any valid topological bits. The correct operation is `sd_flags &= TOPOLOGY_SD_FLAGS`, which performs a bitwise AND with the set of valid topology flags, zeroing out everything else.

However, the buggy code performs `sd_flags &= ~TOPOLOGY_SD_FLAGS`, which is the bitwise AND with the *complement* of the topology flags. This zeros out all valid topology bits and preserves only the invalid non-topological bits — the exact opposite of the intended behavior.

For example, if `sd_flags` were `SD_SHARE_CPUCAPACITY | SD_BALANCE_NEWIDLE` (where `SD_BALANCE_NEWIDLE` is non-topological), the buggy correction would result in `sd_flags = SD_BALANCE_NEWIDLE` (keeping the bad flag, removing the good one). The correct result should be `sd_flags = SD_SHARE_CPUCAPACITY` (keeping the good flag, removing the bad one).

The resulting `sd_flags` (now containing only non-topological flags) is then OR'd with `dflags` and incorporated into the sched_domain's `.flags` field, causing incorrect scheduling behavior for that domain level.

## Consequence

When triggered, this bug causes the sched_domain at the affected topology level to be initialized with incorrect flags. The consequences depend on which non-topological flags were erroneously present and which topological flags were incorrectly stripped:

1. **Lost topological flags**: Flags like `SD_SHARE_CPUCAPACITY` (indicating SMT siblings sharing capacity), `SD_SHARE_PKG_RESOURCES` (indicating shared LLC), or `SD_NUMA` (indicating NUMA boundaries) would be stripped from the domain. This could cause the scheduler to fail to recognize CPU capacity sharing, ignore NUMA topology boundaries, or skip asymmetric packing optimizations. For example, losing `SD_SHARE_CPUCAPACITY` on an SMT system would cause the scheduler to treat SMT siblings as independent full-capacity CPUs, leading to suboptimal task placement.

2. **Retained non-topological flags**: Flags like `SD_BALANCE_NEWIDLE`, `SD_BALANCE_EXEC`, `SD_BALANCE_FORK`, `SD_WAKE_AFFINE`, `SD_SERIALIZE`, or `SD_PREFER_SIBLING` could be added to a topology level where they don't belong. These flags control load balancing behavior, and having them at incorrect topology levels could cause unexpected balancing decisions, excessive migrations, or serialization bottlenecks.

3. **Silent corruption**: The `WARN_ONCE` fires only once, so subsequent domain rebuilds would silently continue with the incorrect correction. The warning message itself ("wrong sd_flags in topology description") would alert administrators, but the actual correction would make things worse, not better.

In practice, this bug is unlikely to trigger on mainstream x86 or ARM configurations because the default topology levels return only valid topology flags. It would primarily affect systems with custom or architecture-specific topology definitions that inadvertently include non-topological flags. However, when triggered, the impact would be significant — potentially causing the scheduler to misidentify the hardware topology and make incorrect placement and balancing decisions.

## Fix Summary

The fix is a single-character change: replacing `~` (bitwise NOT) with nothing in the masking operation:

```c
// Before (buggy):
sd_flags &= ~TOPOLOGY_SD_FLAGS;

// After (fixed):
sd_flags &= TOPOLOGY_SD_FLAGS;
```

This corrects the bitwise AND operation so that it keeps only the bits that are in `TOPOLOGY_SD_FLAGS` (the valid topology flags) and clears all other bits (the invalid non-topological flags). This matches the original intent of the safety check: detect non-topological flags via the `WARN_ONCE`, then strip them to prevent them from propagating into the sched_domain's flags.

The fix is correct and complete because it precisely addresses the single logic error. The detection condition (`sd_flags & ~TOPOLOGY_SD_FLAGS`) was already correct — it properly identifies when non-topological flags are present. Only the correction action was wrong, and this one-character fix makes the correction action match the detection condition's intent. After this fix, any non-topological flags returned by `tl->sd_flags()` will be properly stripped, and only valid topology flags will survive to be incorporated into the sched_domain.

The patch was reviewed by Vincent Guittot and Valentin Schneider, both experienced scheduler developers, confirming the fix's correctness.

## Triggering Conditions

To trigger this bug, the following conditions must be met:

1. **Non-topological flags from `sd_flags()` callback**: A topology level's `sd_flags()` callback must return at least one flag that is not in the `TOPOLOGY_SD_FLAGS` set. The topology flags are: `SD_SHARE_CPUCAPACITY`, `SD_SHARE_PKG_RESOURCES`, `SD_NUMA`, `SD_ASYM_PACKING`, and `SD_SHARE_POWERDOMAIN`. Any other `SD_*` flag (e.g., `SD_BALANCE_NEWIDLE`, `SD_BALANCE_EXEC`, `SD_SERIALIZE`, `SD_WAKE_AFFINE`, `SD_PREFER_SIBLING`) would constitute a non-topological flag.

2. **Sched domain initialization**: The `sd_init()` function must be called, which happens during `build_sched_domains()`. This occurs at boot time during scheduler initialization and whenever sched domains are rebuilt (e.g., CPU hotplug, cpuset changes, or explicit domain rebuild).

3. **Architecture or custom topology**: The default kernel topology levels (defined in `kernel/sched/topology.c`'s `default_topology[]` array) return correct flags. Triggering the bug requires either a custom architecture that overrides the topology definition with incorrect `sd_flags()` callbacks, or a kernel modification that adds non-topological flags to an existing callback.

4. **Kernel version**: The bug exists from v3.16-rc1 (commit `143e1e28cb40`) through v5.8 inclusive. It is fixed in v5.9-rc1 and all subsequent kernels.

The bug is deterministic once the condition is met — every sched domain rebuild will trigger the incorrect flag correction. There are no race conditions or timing dependencies. The probability of hitting this bug depends entirely on whether the running kernel has a topology level that returns non-topological flags.

## Reproduce Strategy (kSTEP)

This bug **cannot** be reproduced with kSTEP for the following reasons:

1. **KERNEL VERSION TOO OLD**: The fix was merged in v5.9-rc1 (commit `9b1b234bb86bcdcdb142e900d39b599185465dbb` is tagged at `v5.8-rc1` and ships in `v5.9-rc1`). kSTEP supports Linux v5.15 and newer only. Since the bug was fixed before v5.15, all kernel versions supported by kSTEP already contain the fix. There is no kSTEP-compatible kernel version that exhibits this bug.

2. **Bug characteristics**: Even if the kernel version were supported, reproducing this bug would require injecting non-topological flags into a topology level's `sd_flags()` callback. This could theoretically be done with kSTEP's `KSYM_IMPORT` to access the `default_topology[]` or `sched_domain_topology` array and replace an `sd_flags` function pointer, then trigger a domain rebuild via `kstep_topo_apply()`. After the rebuild, one would read the resulting `sched_domain` flags (via `cpu_rq(cpu)->sd` chain) to verify whether non-topological flags persisted (buggy) or were stripped (fixed).

3. **What would be needed to support this**: To reproduce this bug on a pre-v5.9 kernel, kSTEP would need to support kernels in the v3.16–v5.8 range. No framework changes to kSTEP's API are required — the existing `KSYM_IMPORT`, `kstep_topo_apply()`, and internal access via `cpu_rq()` would be sufficient to construct a reproducer if the kernel version were in range.

4. **Alternative reproduction methods**: Outside of kSTEP, this bug could be reproduced by:
   - Building a v5.8 or earlier kernel with a custom architecture topology definition that returns a non-topological flag (e.g., modifying the x86 `default_topology[]` to have an `sd_flags()` callback returning `SD_BALANCE_NEWIDLE`).
   - Booting the kernel and checking `dmesg` for the "wrong sd_flags in topology description" warning.
   - Inspecting `/proc/sys/kernel/sched_domain/cpu*/domain*/flags` to verify that the non-topological flag persists (buggy) or is stripped (fixed).
   - Alternatively, a simple code inspection confirms the bug — the bitwise operation `&= ~TOPOLOGY_SD_FLAGS` is clearly wrong when the intent is to keep only topology flags.
