# sched/psi: Fix psi_seq initialization

- **Commit:** 99b773d720aeea1ef2170dce5fcfa80649e26b78
- **Affected file(s):** kernel/sched/psi.c
- **Subsystem:** PSI (Pressure Stall Information)

## Bug Description

When psi_seq (a per-CPU seqcount) was moved from per-group ownership to a global per-CPU variable, the group_init() function continued to re-initialize the seqcount whenever a new PSI group was created. This causes seqcount corruption because the global seqcount is shared across multiple groups and should not be reinitialized after initial boot-time setup.

## Root Cause

The previous commit (570c8efd5eb7) refactored psi_seq from a per-group seqcount to a global per-CPU seqcount for optimization purposes. However, the group_init() function was not updated to reflect this ownership change. When group_init() called seqcount_init() on the global psi_seq for each possible CPU during group creation, it overwrote the seqcount state that was already in use by other groups, leading to synchronization corruption.

## Fix Summary

The fix initializes psi_seq once at declaration time using SEQCNT_ZERO(psi_seq) and removes the re-initialization loop from group_init(). This ensures the global per-CPU seqcount is only initialized once during kernel boot, preventing corruption when subsequent PSI groups are created.

## Triggering Conditions

This bug is triggered when creating multiple PSI groups (e.g., via cgroup creation) after kernel boot. The global per-CPU `psi_seq` seqcount gets corrupted because `group_init()` calls `seqcount_init()` on the already-in-use global seqcount for each possible CPU. The corruption occurs when:
- PSI is enabled and functioning (reading/writing pressure metrics)
- A new cgroup is created, which calls `group_init()` for the new PSI group
- `seqcount_init()` resets the sequence counter to 0 while readers/writers may be using it
- Concurrent readers using `psi_read_begin()`/`psi_read_retry()` can get inconsistent sequence numbers
- This leads to seqcount validation failures and potential data races in PSI metric collection

## Reproduce Strategy (kSTEP)

Create concurrent PSI activity while dynamically creating cgroups to trigger seqcount re-initialization:
- Use 2+ CPUs to enable per-CPU seqcount usage across multiple CPUs
- In `setup()`: Create background tasks to generate memory/CPU pressure using `kstep_task_create()` and `kstep_task_wakeup()`
- Use `kstep_cgroup_create()` to create multiple cgroups dynamically during PSI metric collection
- In `run()`: Start pressure-generating tasks, then repeatedly call `kstep_cgroup_create("test_group_N")` in a loop
- Use callbacks like `on_tick_begin()` to verify PSI metrics are being collected and detect seqcount inconsistencies
- Monitor for seqcount corruption by checking if PSI reads become inconsistent or hang due to sequence number mismatches
- Look for kernel warnings about seqcount validation failures in the logs via `TRACE_INFO()` output
