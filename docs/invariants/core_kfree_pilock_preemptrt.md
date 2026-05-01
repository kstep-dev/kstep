# kfree under pi_lock on PREEMPT_RT
**Source bug:** `9a5418bc48babb313d2a62df29ebe21ce8c06c59`

No generic invariant applicable. This is a locking-context violation (calling potentially-sleeping `kfree()` under raw spinlock on PREEMPT_RT), not a scheduler state invariant — no scheduler data structure is left in an inconsistent state, so there is no predicate over scheduler state to check.
