# Overutilized New Task No-Op
**Source bug:** `8e1ac4299a6e8726de42310d9c1379f188140c71`

No generic invariant applicable. The bug is a variable-clobbering logic error where the `flags` parameter is overwritten mid-function before a later conditional check, making that check a no-op — this is a one-off control-flow mistake specific to `enqueue_task_fair()` internals, not a violation of a reusable scheduler state property.
