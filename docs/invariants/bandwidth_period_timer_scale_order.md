# Bandwidth Period Timer Scale Order
**Source bug:** `5a6d6a6ccb5f48ca8cf7c6d64ff83fd9c7999390`

No generic invariant applicable. Bug is an operation ordering error within a single function (`sched_cfs_period_timer`): quota/period scaling was done between timer forwarding and runtime refill, granting excess runtime for one iteration. This is an ad-hoc code ordering fix, not a checkable state invariant at standard hook points.
