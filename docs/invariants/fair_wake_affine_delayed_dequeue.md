# Fair wake_affine Delayed Dequeue
**Source bug:** `aa3ee4f0b7541382c9f6f43f7408d73a5d4f4042`

No generic invariant applicable. The bug is a specific function (`wake_affine_idle`) using the wrong counter (`rq->nr_running` instead of the effective running count minus delayed tasks); this is a "use the right metric" coding error in one call site, not a violation of a checkable state invariant.
