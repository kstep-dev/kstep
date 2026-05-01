# Stopper Double-Submission Guard
**Source bug:** `9e81889c7648d48dd5fe13f41cbc99f3c362484a`

No generic invariant applicable. Race condition involving double-submission of a `cpu_stop_work` to the stopper list — the corruption is in stopper internals, not expressible as a predicate over scheduler runqueue/entity state.
