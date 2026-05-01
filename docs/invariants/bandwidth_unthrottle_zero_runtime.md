# Unthrottle Walk Requires Positive Runtime
**Source bug:** `956dfda6a70885f18c0f8236a461aa2bc4f556ad`

No generic invariant applicable. The bug is a one-off initialization error in `tg_set_cfs_bandwidth()` that set `runtime_remaining = 0` instead of `1` for an unthrottled cfs_rq; the natural invariant "unthrottled + runtime_enabled ⇒ runtime_remaining > 0" does not hold in general during normal operation (runtime legitimately reaches 0 before the throttle check fires), making it uncheckable as a continuous invariant without false positives.
