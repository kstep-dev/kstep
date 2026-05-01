# NULL mm Dereference in task_tick_numa
**Source bug:** `b3f9916d81e8ffb21cbe7abccf63f86a5a1d598a`

No generic invariant applicable. The fix is a missing NULL-pointer guard (`!curr->mm`) before dereferencing `task->mm` in a NUMA balancing path — a defensive programming issue, not a checkable scheduler state invariant.
