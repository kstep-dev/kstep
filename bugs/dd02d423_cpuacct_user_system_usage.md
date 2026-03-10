# sched/cpuacct: Fix user/system in shown cpuacct.usage*

- **Commit:** dd02d4234c9a2214a81c57a16484304a1a51872a
- **Affected file(s):** kernel/sched/cpuacct.c
- **Subsystem:** cpuacct

## Bug Description

When using KVM guests or other workloads, the `cpuacct_charge()` function incorrectly accounts most of the guest time as system time. This mismatch means that the `cpuacct.usage_sys`, `cpuacct.usage_user`, and related files show inconsistent values compared to `cpuacct.stat` or `/proc/<pid>/stat`, causing user-visible accounting errors.

## Root Cause

The `cpuacct_charge()` function attempted to classify execution time as user or system mode by inspecting CPU registers with `user_mode(regs)`. However, this register-based classification is unreliable for certain workloads such as KVM guests, leading to systematic misclassification of time. The underlying data structure (`struct cpuacct_usage` with separate user/system fields) was being updated with incorrect classifications.

## Fix Summary

The fix separates the concerns: `cpuacct_charge()` now accounts only the total execution time (into a single `u64` field), while `cpuacct_cpuusage_read()` derives user and system times from the `cpustat` array that is properly maintained by `cpuacct_account_field()`. This ensures user/system accounting is consistent across all interfaces by using the same authoritative source.

## Triggering Conditions

The bug manifests when tasks execute in contexts where `user_mode(get_irq_regs())` or `user_mode(task_pt_regs(tsk))` returns incorrect classifications. This primarily occurs with:
- KVM guest workloads where guest time is misclassified as system time
- Tasks that frequently transition between user/kernel modes
- IRQ contexts where register state is unreliable
- The bug requires a cpuacct cgroup to be active and tasks performing significant CPU work
- Race conditions aren't necessary - the misclassification is systematic due to register inspection logic

## Reproduce Strategy (kSTEP)

Create a CPU-intensive workload in a cpuacct cgroup and compare user/system accounting between `cpuacct.usage_sys`/`cpuacct.usage_user` vs `cpuacct.stat`:
- Use 2+ CPUs (CPU 0 reserved for driver)  
- In setup(): Create cpuacct cgroup with `kstep_cgroup_create("test")`, create CPU-bound task with `kstep_task_create()`
- In run(): Add task to cgroup with `kstep_cgroup_add_task("test", task->pid)`, run intensive workload with `kstep_tick_repeat(1000)`
- Use callbacks to periodically read `/sys/fs/cgroup/cpuacct/test/cpuacct.usage_sys` and `/sys/fs/cgroup/cpuacct/test/cpuacct.stat`
- Detect bug by comparing user/system ratios between the two accounting methods - significant divergence indicates the bug
- Simulate guest-like context by creating tasks that frequently enter kernel mode (e.g., via system calls)
