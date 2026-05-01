# wait_task_inactive sched_delayed
**Source bug:** `b7ca5743a2604156d6083b88cefacef983f3a3a6`

No generic invariant applicable. This is a semantic mismatch where an existing function (wait_task_inactive) didn't account for the new sched_delayed state — a missing case handler, not a violated state property. No scheduler state invariant is broken: the task IS physically on the runqueue, and task_on_rq_queued() correctly returns true. The fix adds special-case handling (force dequeue) rather than restoring a violated invariant.
