# is_core_idle Wrong Variable
**Source bug:** `1c6829cfd3d5124b125e6df41158665aea413b35`

No generic invariant applicable. This is a copy-paste/wrong-variable typo (`cpu` vs `sibling` in a loop) — a one-off coding error with no reusable scheduler state property to check.
