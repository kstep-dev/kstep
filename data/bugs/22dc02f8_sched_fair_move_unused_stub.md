# Revert "sched/fair: Move unused stub functions to header"

- **Commit:** 22dc02f81cddd19528fc1d4fbd7404defbf736c5
- **Affected file(s):** kernel/sched/fair.c, kernel/sched/sched.h
- **Subsystem:** CFS, core scheduling

## Bug Description

Commit 7aa55f2a5902 ("sched/fair: Move unused stub functions to header") had incorrect patch content. While its changelog claimed to move stub functions to a header, the actual patch inadvertently reverted four prior patches, removing important function declarations and visibility guards. This caused compilation issues and missing function prototypes that should have been exposed.

## Root Cause

The problematic commit (7aa55f2a5902) performed a "revert of a revert" by accident—its patch content actually reverted these four prior patches that provided necessary function declarations with proper conditional compilation guards:
- f7df852ad6db: task_vruntime_update() prototype visibility
- c0bdfd72fbfb: Hide unused init_cfs_bandwidth() stub
- 378be384e01f: schedule_user() declaration  
- d55ebae3f312: Hide unused sched_update_scaling()

## Fix Summary

This fix re-applies those four reverted patches correctly. It restores the necessary function declarations (task_vruntime_update, schedule_user) in the header file, adds proper CONFIG_SMP and CONFIG_FAIR_GROUP_SCHED guards around function definitions, and ensures correct visibility of sched-related functions across compilation configurations.

## Triggering Conditions

This is a build-time bug that manifests during kernel compilation when specific CONFIG options are set. The bug occurs when:
- CONFIG_SMP is disabled, making sched_update_scaling() unused but still visible without proper guards
- CONFIG_FAIR_GROUP_SCHED is disabled, making init_cfs_bandwidth() stub unnecessary but exposed  
- External code tries to call task_vruntime_update() or schedule_user() but their declarations are missing from headers
- The compiler encounters undefined function references or unused function warnings/errors
- Specific build configurations (like certain embedded or non-SMP configs) that don't include all scheduler subsystems

## Reproduce Strategy (kSTEP)

This bug cannot be reproduced using kSTEP since it's a compilation issue, not a runtime scheduler behavior bug. kSTEP tests runtime scheduler behavior, but this fix addresses build-time symbol visibility and conditional compilation guards.

To reproduce this bug, one would need to:
1. Check out kernel version with the problematic commit 7aa55f2a5902  
2. Configure kernel build with CONFIG_SMP=n or specific configs that expose the missing declarations
3. Attempt kernel compilation and observe build failures due to missing function prototypes
4. Verify the fix by applying commit 22dc02f81cddd19528fc1d4fbd7404defbf736c5 and confirming successful compilation

Since kSTEP operates on already-compiled kernels, it cannot detect or reproduce build-time symbol visibility issues.
