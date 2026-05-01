# Deadline Empty Cpuset Crash
**Source bug:** `b6e8d40d43ae4dec00c8fea2593eeea3114b8f44`

No generic invariant applicable. This is an input validation bug — an out-of-bounds CPU index from `cpumask_any_and()` on an empty mask is passed to `dl_bw_of()`, not a scheduler state consistency violation.
