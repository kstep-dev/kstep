# Fair TTWU Move Affine Miscount
**Source bug:** `39afe5d6fc59237ff7738bf3ede5a8856822d59d`

No generic invariant applicable. This is a one-off statistics accounting logic error where an incomplete sentinel-value check miscounted non-affine wakeups as affine; the fix is purely a conditional correction (`target == nr_cpumask_bits` → `target != this_cpu`) with no broader schedulable-state invariant violated.
