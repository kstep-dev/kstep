# NUMA Swap Task NULL mm
**Source bug:** `db6cc3f4ac2e6cdc898fc9cbc8b32ae1bf56bdad`

No generic invariant applicable. This is a TOCTOU race where newly added statistics code dereferences `p->mm` without holding `task_lock()` after the task has exited; the fix is a full revert of the feature, not a scheduler state property that can be expressed as a checkable invariant.
