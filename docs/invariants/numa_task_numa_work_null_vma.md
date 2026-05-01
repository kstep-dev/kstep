# NULL VMA Dereference in task_numa_work
**Source bug:** `9c70b2a33cd2aa6a5a59c5523ef053bd42265209`

No generic invariant applicable. This is a one-off coding pattern bug (do-while loop executing body before NULL check on VMA pointer), not a scheduler state consistency violation.
