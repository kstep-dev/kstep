# Asymmetric Wakeup Prev Skip
**Source bug:** `b4c9c9f15649c98a5b45408919d1ff4fd7f5531c`

No generic invariant applicable. This is a control-flow ordering bug in `select_idle_sibling()` where a new asymmetric code path bypassed existing prev/target/recent_used_cpu heuristics — it's a one-off logic structuring error with no corresponding scheduler state predicate to check.
