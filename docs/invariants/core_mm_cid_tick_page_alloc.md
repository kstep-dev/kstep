# No Generic Invariant — Sleeping-in-Atomic Context (KASAN + PREEMPT_RT)
**Source bug:** `73ab05aa46b02d96509cb029a8d04fca7bbde8c7`

No generic invariant applicable. Bug is a locking-context violation (sleeping allocation under raw_spinlock) specific to CONFIG_KASAN + CONFIG_PREEMPT_RT; it does not violate any predicate over scheduler state and is not observable through scheduling behavior.
