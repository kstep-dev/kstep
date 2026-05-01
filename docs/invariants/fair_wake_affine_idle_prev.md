# Wake Affine Idle Prev
**Source bug:** `d8fcb81f1acf651a0e50eacecca43d0524984f87`

No generic invariant applicable. The bug is a missing case in a task-placement heuristic (`wake_affine_idle` failing to short-circuit for an idle cross-socket `prev_cpu`), manifesting as a performance regression rather than a violation of checkable scheduler state; no structural invariant over `rq`/`cfs_rq`/`se` fields is violated.
