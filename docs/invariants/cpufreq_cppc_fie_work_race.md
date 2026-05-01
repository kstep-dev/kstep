# CPPC FIE Work Race Invariant
**Source bug:** `771fac5e26c17845de8c679e6a947a4371e86ffc`

No generic invariant applicable. Bug is a driver-level use-after-free race (async work not cancelled during cpufreq policy teardown) entirely in cppc_cpufreq.c, not a scheduler state invariant violation.
