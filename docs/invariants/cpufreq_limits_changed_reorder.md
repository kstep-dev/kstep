# Cpufreq Limits Changed Reorder
**Source bug:** `79443a7e9da3c9f68290a8653837e23aba0fa89f`

No generic invariant applicable. Bug is a CPU memory reordering issue (missing memory barriers and READ_ONCE/WRITE_ONCE on a cross-CPU flag); no scheduler state predicate can detect incorrect store/load ordering.
