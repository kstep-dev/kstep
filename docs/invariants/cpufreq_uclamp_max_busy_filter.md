# Cpufreq Uclamp Max Busy Filter
**Source bug:** `7a17e1db1265471f7718af100cfc5e41280d53a7`

No generic invariant applicable. Bug is in cpufreq governor heuristic logic (busy filter overriding uclamp_max), not a scheduler state invariant — requires cpufreq subsystem infrastructure unavailable in typical scheduler state checks.
