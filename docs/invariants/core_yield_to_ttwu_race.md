# CFS Buddy Pointer Consistency
**Source bug:** `5d808c78d97251af1d3a3e4f253e7d6c39fd871e`

No generic invariant applicable. This is a locking protocol violation (missing pi_lock in yield_to) causing a transient race; the kernel already checks the observable symptom via SCHED_WARN_ON(!se->on_rq) in set_next_buddy(), and no additional point-in-time state invariant would reliably catch the transient inconsistency during the tiny race window.
