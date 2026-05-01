# Capacity Inversion Self-Comparison and RCU Protection
**Source bug:** `da07d2f9c153e457e845d4dcfdd13568d71d18a4`

No generic invariant applicable. The three sub-bugs (wrong guard condition, missing RCU lock, self-comparison in PD loop) are ad-hoc logic/locking errors specific to the capacity inversion detection feature, not violations of a reusable scheduler state property.
