# ILB Spurious need_resched Invariant
**Source bug:** `ff47a0acfcce309cf9e175149c75614491953c8f`

No generic invariant applicable. The bug is a false-positive `need_resched()` check caused by TIF_NEED_RESCHED being overloaded by an IPI optimization in one specific code path (`_nohz_idle_balance`); the fix adds an `idle_cpu()` guard that is specific to that bailout logic and does not generalize to a reusable scheduler-wide property.
