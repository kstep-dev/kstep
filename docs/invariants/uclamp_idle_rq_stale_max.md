# Uclamp Idle Rq Stale Max Aggregation
**Source bug:** `3e1493f46390618ea78607cb30c58fc19e2a5035`

No generic invariant applicable. The bug is a logic error in a single utility function (`uclamp_rq_util_with()`) that incorrectly aggregates stale rq-level uclamp values with a task's values on an idle rq — no scheduler state invariant is violated; the rq state itself is consistent (idle flag set, stale values retained by design), the error is purely in how the function interprets that state.
