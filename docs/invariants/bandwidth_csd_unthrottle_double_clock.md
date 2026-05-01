# Bandwidth CSD Unthrottle Double Clock
**Source bug:** `ebb83d84e49b54369b0db67136a5fe1087124dcc`

No generic invariant applicable. The kernel already enforces this exact property via the `WARN_DOUBLE_CLOCK` / `RQCF_UPDATED` debug check in `update_rq_clock()`; the bug is simply a new code path that failed to use the existing `RQCF_ACT_SKIP` suppression mechanism around a loop.
