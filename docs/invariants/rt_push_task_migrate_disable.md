# get_push_task() migrate_disable Check
**Source bug:** `e681dcbaa4b284454fecd09617f8b24231448446`

No generic invariant applicable. Bug is a missing predicate check (`migration_disabled`) in a single helper function (`get_push_task()`), specific to PREEMPT_RT kernels; the consequence is only unnecessary stopper thread invocation with no observable state corruption.
