# Energy feec uclamp_min skip
**Source bug:** `d81304bc6193554014d4372a01debdf65e1e9a4d`

No generic invariant applicable. The bug is a missing uclamp consideration in a specific early-exit optimization inside `find_energy_efficient_cpu()` — a code-level logic error in one function's shortcut path, not a violation of an observable scheduler state invariant. The property "use clamped utilization, not raw PELT utilization, for zero-checks" is a coding guideline rather than a checkable predicate over scheduler data structures.
