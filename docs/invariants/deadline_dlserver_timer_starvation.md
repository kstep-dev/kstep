# DL Server Timer Starvation Invariant
**Source bug:** `421fc59cf58c64f898cafbbbbda0bc705837e7df`

No generic invariant applicable. The bug is a logic error in the throttle fallback path where a dl_server is immediately re-enqueued (with fresh budget) instead of remaining throttled with an armed timer; the post-bug state is internally self-consistent (dl_throttled==0, entity enqueued on dl_rq with valid runtime and future deadline), so no point-in-time state predicate distinguishes the buggy state from a legitimately running dl_server.
