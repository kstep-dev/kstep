# Cpufreq HWP Limits Skip
**Source bug:** `d1e7c2996e988866e7ceceb4641a0886885b7889`

No generic invariant applicable. Bug is a missing driver-flag check in cpufreq schedutil's caching logic—specific to cpufreq governor/driver interaction, not expressible as a predicate over core scheduler state (rq, cfs_rq, sched_entity, etc.).
