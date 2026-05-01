# util_est Boost Double-Count
**Source bug:** `c2e164ac33f75e0acb93004960c73bd9166d3d35`

No generic invariant applicable. The bug is a logic error in the `cpu_util()` projection helper that incorrectly mixed two incompatible signals (`runnable_avg` which includes decaying blocked-task contributions, and `util_est.enqueued` which is a discrete snapshot of only enqueued tasks); it violates no scheduler state invariant — the actual `cfs_rq` fields are always correct, and the error exists only in a transient local computation within one function.
