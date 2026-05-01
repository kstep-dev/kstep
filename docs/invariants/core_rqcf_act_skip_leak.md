# RQCF_ACT_SKIP Flag Leak
**Source bug:** `5ebde09d91707a4a9bec1e3d213e3c12ffde348f`

No generic invariant applicable. The bug is a race condition where a temporary flag (`RQCF_ACT_SKIP`) leaks across a lock-drop window in `__schedule()`; the kernel already contains the exact invariant check (`SCHED_WARN_ON(rq->clock_update_flags & RQCF_ACT_SKIP)` in `rq_clock_start_loop_update()`), and the fix is simply moving the flag-clear earlier — this is too specific to the `RQCF_ACT_SKIP` lifecycle to generalize.
