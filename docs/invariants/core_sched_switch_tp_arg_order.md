# Tracepoint Argument Order ABI
**Source bug:** `9c2136be0878c88c53dea26943ce40bb03ad8d8d`

No generic invariant applicable. This is a tracepoint ABI compatibility issue (argument reordering breaking BPF consumers), not a scheduler state invariant violation — no scheduler-internal predicate over rq/task_struct/sched_entity is violated.
