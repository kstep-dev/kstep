# sched_group_cookie_match Wrong RQ
**Source bug:** `e705968dd687574b6ca3ebe772683d5642759132`

No generic invariant applicable. This is a copy-paste variable reference error (using the caller's `rq` instead of `cpu_rq(cpu)` in a loop body); no scheduler state invariant is violated that could be generically checked — the bug is a one-off coding mistake with no reusable predicate over scheduler data structures.
