# Delayed Dequeue TTWU Race
**Source bug:** `b55945c500c5723992504aa03b362fab416863a6`

No generic invariant applicable. This is a use-after-release race condition (code ordering bug in `__block_task()` where `p->on_rq = 0` was written too early, allowing ttwu to migrate the task while the releasing CPU still referenced `@p`); the violated property is a memory-ordering/ownership contract, not an observable scheduler state invariant checkable at any hook point.
