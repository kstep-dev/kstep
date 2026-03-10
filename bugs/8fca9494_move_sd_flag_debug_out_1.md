# sched/topology: Move sd_flag_debug out of linux/sched/topology.h

- **Commit:** 8fca9494d4b4d6b57b1398cd473feb308df656db
- **Affected file(s):** kernel/sched/debug.c, include/linux/sched/topology.h
- **Subsystem:** topology

## Bug Description

The sd_flag_debug array was defined in a header file (linux/sched/topology.h) that is included in multiple translation units throughout the kernel. This violates the C One Definition Rule (ODR), causing each translation unit that includes the header to have its own independent copy of the array, potentially leading to linker errors, binary bloat, and inconsistencies between different parts of the code referencing the "same" array.

## Root Cause

The previous commit b6e862f38672 introduced the sd_flag_debug array definition directly in a header file using a macro-based approach. When this header is included in multiple .c files, each compilation unit gets its own copy of the array data, violating ODR. The C linker may treat this as a multiple definition error or silently create separate instances, leading to correctness and maintenance issues.

## Fix Summary

The fix moves the sd_flag_debug array definition from the header file (linux/sched/topology.h) to a single source file (kernel/sched/debug.c), while leaving only an extern declaration in the header. This ensures there is exactly one definition of the array, eliminating the ODR violation and potential link-time issues.

## Triggering Conditions

This ODR violation manifests at compile/link time rather than runtime:
- Multiple translation units include linux/sched/topology.h when CONFIG_SCHED_DEBUG is enabled
- Each inclusion creates a separate instance of the sd_flag_debug array definition
- Linkers may report "multiple definition" errors or silently create duplicate symbols
- Different object files may reference different instances of the "same" array
- Modern compilers with strict ODR checking (like recent GCC/Clang) are more likely to detect this
- The issue becomes apparent when linking multiple scheduler-related object files together

## Reproduce Strategy (kSTEP)

This is primarily a build-time issue, but kSTEP can verify the fix by testing sd_flag_debug access:
- Use 2+ CPUs to ensure scheduler domain creation across multiple CPUs
- Call kstep_topo_init() and kstep_topo_apply() to set up scheduler domains
- Access sd_flag_debug array through scheduler debugging interfaces (if available in kernel)
- Create scenarios that would trigger scheduler domain debugging output
- Use kstep_print_sched_debug() to exercise code paths that reference sd_flag_debug
- In the buggy kernel, inconsistent behavior could manifest if different parts of the scheduler
  reference different array instances (though this would be subtle and rare)
- The main verification is ensuring the kernel builds and links without multiple definition errors
