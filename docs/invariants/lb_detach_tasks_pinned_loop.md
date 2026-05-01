# detach_tasks Pinned Loop Invariant
**Source bug:** `2feab2492deb2f14f9675dd6388e9e2bf669c27a`

No generic invariant applicable. The bug is an unbounded iteration (O(n) under rq lock) caused by conditionally bypassing a loop limit — a performance/liveness issue, not a scheduler state consistency violation.
