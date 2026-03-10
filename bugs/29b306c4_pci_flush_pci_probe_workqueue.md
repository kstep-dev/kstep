# PCI: Flush PCI probe workqueue on cpuset isolated partition change

- **Commit:** 29b306c44eb5eefdfa02d6ba1205f479f82fb088
- **Affected file(s):** kernel/sched/isolation.c
- **Subsystem:** core

## Bug Description

When the HK_TYPE_DOMAIN housekeeping cpumask is modified at runtime to isolate CPUs, PCI probe work may still be pending or actively executing on CPUs that are being removed from the housekeeping mask. This creates a race condition where asynchronous PCI probing interferes with CPU isolation semantics, potentially causing synchronization issues and breaking the guarantees of housekeeping-isolated CPUs.

## Root Cause

The housekeeping subsystem's `housekeeping_update()` function performs RCU synchronization to update the HK_TYPE_DOMAIN cpumask, but does not flush PCI probe work that is queued on the main per-CPU workqueue pool. Without this flush, PCI probe work scheduled before the isolation can continue executing on newly-isolated CPUs, violating the isolation contract.

## Fix Summary

The fix adds a call to `pci_probe_flush_workqueue()` immediately after RCU synchronization in the `housekeeping_update()` function. This ensures all PCI probe work is completed before the housekeeping cpumask change takes effect, maintaining proper synchronization between PCI probing and CPU isolation.

## Triggering Conditions

This bug requires runtime modification of the HK_TYPE_DOMAIN housekeeping cpumask (CPU isolation changes) while PCI probe work is queued or executing. The race occurs when:
- Asynchronous PCI device probing is scheduled on CPUs via the system workqueue
- A concurrent cpuset isolated partition change calls `housekeeping_update()` to modify the HK_TYPE_DOMAIN cpumask
- The RCU synchronization completes but PCI probe work continues on newly-isolated CPUs
- Without the flush, PCI probe callbacks execute on CPUs that should be isolated from kernel work
- This violates CPU isolation guarantees and can interfere with real-time/isolated workloads

## Reproduce Strategy (kSTEP)

Requires at least 3 CPUs (CPU 0 reserved). Setup multi-CPU topology with CPU isolation:
- Use `kstep_topo_init()` and `kstep_topo_apply()` to configure CPU topology
- Create cgroup with CPU isolation using `kstep_cgroup_create()` and `kstep_cgroup_set_cpuset()`
- Simulate PCI probe work by creating workqueue tasks with `kstep_kthread_create()` and pin to target CPUs
- Use `on_tick_begin()` callback to monitor workqueue activity on isolated CPUs
- Trigger housekeeping mask change by modifying cgroup cpuset configuration mid-execution
- Detect bug by observing kernel work executing on CPUs that should be isolated after the mask update
- Log CPU isolation violations and workqueue activity to verify the race condition occurred
