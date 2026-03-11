# mm: move node_reclaim_distance to fix NUMA without SMP

- **Commit:** 61bb6cd2f765b90cfc5f0f91696889c366a6a13d
- **Affected file(s):** kernel/sched/topology.c
- **Subsystem:** topology

## Bug Description

The kernel fails to build when CONFIG_NUMA=y but CONFIG_SMP=n with an undefined reference error to `node_reclaim_distance`. This occurs because the variable declaration was located in an SMP-only section of the scheduler topology code, making it unavailable when SMP is disabled despite being needed by the NUMA memory allocation code.

## Root Cause

The `node_reclaim_distance` variable was declared inside the CONFIG_NUMA block in kernel/sched/topology.c, which is itself compiled only when SMP is enabled. However, the mm/page_alloc.c code that references this variable is compiled whenever CONFIG_NUMA is enabled, regardless of the SMP configuration. This mismatch causes a linker error when NUMA is used without SMP.

## Fix Summary

Move the `node_reclaim_distance` variable declaration from kernel/sched/topology.c (SMP-only file) to a generic file that is compiled whenever CONFIG_NUMA is enabled, ensuring the symbol is available for the NUMA memory allocation code regardless of the SMP configuration.

## Triggering Conditions

This is a build-time configuration issue triggered by specific kernel config combinations:
- CONFIG_NUMA=y must be enabled (enables NUMA memory allocation code in mm/page_alloc.c)
- CONFIG_SMP=n must be disabled (excludes kernel/sched/topology.c from compilation)  
- The `node_reclaim_distance` variable was declared in topology.c but referenced by page_alloc.c
- SuperH architecture with migor_defconfig exemplifies this configuration scenario
- Linker fails during build with "undefined reference to node_reclaim_distance" error
- The `get_page_from_freelist()` function in mm/page_alloc.c calls `zone_allows_reclaim()` which references the missing symbol

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced with kSTEP since it is a build-time linker error, not a runtime scheduler behavior issue. To reproduce this bug, one would need to:
- Configure a kernel build with CONFIG_NUMA=y and CONFIG_SMP=n (e.g., SuperH migor_defconfig before the fix)
- Attempt to compile the kernel with a buggy commit before 61bb6cd2f765
- Observe the linker error during the build process, not runtime execution
- The bug manifests as a missing symbol error preventing successful kernel compilation
- No runtime testing or kSTEP driver execution is applicable since the kernel fails to build
- This is fundamentally a configuration dependency issue rather than scheduler logic bug
