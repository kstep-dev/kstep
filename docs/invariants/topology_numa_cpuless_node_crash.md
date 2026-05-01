# NUMA Cpuless Node Crash
**Source bug:** `617f2c38cb5ce60226042081c09e2ee3a90d03f8`

No generic invariant applicable. The bug is a missing input validation (NULL-guard for CPU-less NUMA node) in a specific topology lookup function — not a violation of a general scheduler state property.
