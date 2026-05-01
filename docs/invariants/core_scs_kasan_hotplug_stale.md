# SCS/KASAN Hotplug Stale State
**Source bug:** `dce1ca0525bfdc8a69a9343bc714fbc19a2f04b3`

No generic invariant applicable. Bug is a CPU hotplug lifecycle issue (stale SCS SP and KASAN shadow not reset on bringup) outside scheduler state — it concerns architecture-specific security features and sanitizer shadow memory, not scheduler-observable predicates.
