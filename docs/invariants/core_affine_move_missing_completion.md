# Affine Move Missing Completion
**Source bug:** `d707faa64d03d26b529cc4aea59dab1b016d4d33`

No generic invariant applicable. This is a missing control-flow branch in `migration_cpu_stop()` — the `task_rq(p) != rq && dest_cpu >= 0 && pending` case was unhandled, leaving a completion unsignaled. The violation (leaked `migration_pending`) is only transiently distinguishable from legitimate in-flight migration states, making it uncheckable as a point-in-time predicate without false positives.
