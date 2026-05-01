# NULL Pointer in Non-SMP sched_setaffinity
**Source bug:** `5657c116783545fb49cd7004994c187128552b12`

No generic invariant applicable. Bug is a compile-time configuration issue (CONFIG_SMP=n) causing a NULL pointer dereference in a code path that doesn't exist in SMP kernels — a one-off logic error in conditional guarding, not a scheduler state invariant violation.
