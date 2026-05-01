# Cpufreq Non-Invariant Frequency Stuck
**Source bug:** `e37617c8e53a1f7fcba6d5e1041f4fd8a2425c27`

No generic invariant applicable. The bug is a logic error in the schedutil governor's frequency selection formula specific to the non-invariant code path—`get_capacity_ref_freq()` returned exactly `policy->cur`, making it algebraically impossible to select a higher OPP. This is an ad-hoc formula bug in the cpufreq subsystem (not core scheduler state), and the affected code path is unreachable in QEMU/kSTEP since no cpufreq driver is present.
