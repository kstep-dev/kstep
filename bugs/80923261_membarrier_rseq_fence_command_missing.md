# Fix membarrier-rseq fence command missing from query bitmask

- **Commit:** 809232619f5b15e31fb3563985e705454f32621f
- **Affected file(s):** kernel/sched/membarrier.c
- **Subsystem:** membarrier

## Bug Description

The membarrier-rseq fence commands (MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ and MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ) were added to the kernel but were not registered in the MEMBARRIER_CMD_BITMASK used by the MEMBARRIER_CMD_QUERY command. This means applications cannot discover whether the membarrier-rseq fence feature is available on the system, even though the commands are implemented and functional.

## Root Cause

The MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ_BITMASK macro was defined but never added to the MEMBARRIER_CMD_BITMASK macro that forms the response to MEMBARRIER_CMD_QUERY. Additionally, a typo in the definition referenced MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ_BITMASK, which doesn't exist as a constant. This combination prevented the rseq fence commands from being reported as available.

## Fix Summary

The fix renames the bitmask constant to remove the misleading CMD prefix (since it's a bitmask, not a command), corrects the typo by using the actual command constant MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ, and adds the MEMBARRIER_PRIVATE_EXPEDITED_RSEQ_BITMASK to the MEMBARRIER_CMD_BITMASK so that MEMBARRIER_CMD_QUERY correctly reports the availability of membarrier-rseq fence functionality.

## Triggering Conditions

This is a static configuration bug in the membarrier subsystem's header definitions. The triggering condition is:
- CONFIG_RSEQ is enabled in the kernel configuration
- Userspace application calls `membarrier(MEMBARRIER_CMD_QUERY, 0)` to discover available commands
- The returned bitmask incorrectly excludes MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ and MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ
- Even though these commands are functional, applications cannot discover their availability
- No specific task states, CPU topology, or race conditions are required - the bug is deterministic in the bitmask computation

## Reproduce Strategy (kSTEP)

This bug involves userspace API discoverability rather than kernel scheduler state, making it challenging for kSTEP:
- Use 2 CPUs minimum (CPU 0 reserved for driver)
- In setup(): Enable CONFIG_RSEQ if not already enabled via kstep_sysctl_write()
- In run(): Call the membarrier system call via inline assembly or access the membarrier_cmd_query function directly
- Check if MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ bits are missing from the returned bitmask
- Use kstep_fail() if the query bitmask doesn't include rseq commands when CONFIG_RSEQ is enabled
- Use kstep_pass() if the fix correctly includes MEMBARRIER_PRIVATE_EXPEDITED_RSEQ_BITMASK in MEMBARRIER_CMD_BITMASK
- Note: This may require kernel-internal access since kSTEP focuses on scheduler state rather than syscall interfaces
