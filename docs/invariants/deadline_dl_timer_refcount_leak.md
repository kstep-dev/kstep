# DL Timer Refcount Leak
**Source bug:** `b58652db66c910c2245f5bee7deca41c12d707b9`

No generic invariant applicable. Bug is a missing `put_task_struct()` in one specific cancel-timer code path — a refcount bookkeeping error that requires shadow state to detect and does not map to a checkable scheduler state predicate.
