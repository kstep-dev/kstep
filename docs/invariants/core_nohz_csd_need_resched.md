# No Generic Invariant for nohz_csd_func need_resched Check
**Source bug:** `ea9cffc0a154124821531991d5afdd7e8b20d7aa`

No generic invariant applicable. The bug is a semantic mismatch where `TIF_NEED_RESCHED` was repurposed by an SMP IPI optimization (commit b2a02fc43a1f) to also mean "process queued call functions," but `nohz_csd_func()` still interpreted it as "a task needs scheduling" — this is an ad-hoc logic error from changed flag semantics across subsystems, not a violation of a checkable scheduler state invariant.
