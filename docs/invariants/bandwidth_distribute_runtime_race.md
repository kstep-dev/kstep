# CFS Bandwidth Runtime Distribution Race
**Source bug:** `26a8b12747c975b33b4a82d62e4a307e1c07f31b`

No generic invariant applicable. This is a TOCTOU race condition (stale snapshot of `cfs_b->runtime` used without holding the lock); the fix is correct locking, and the observable effect (runtime over-allocation) requires tracking cumulative flows over a period rather than checking point-in-time state.
