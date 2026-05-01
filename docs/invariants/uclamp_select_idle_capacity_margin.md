# Uclamp Select Idle Capacity Migration Margin
**Source bug:** `b759caa1d9f667b94727b2ad12589cbc4ce13a82`

No generic invariant applicable. The bug is a code pattern issue (using `fits_capacity()` with uclamp-clamped values instead of `util_fits_cpu()` with separated components); it violates a specification of the fitness function's behavior rather than a checkable state invariant over scheduler data structures.
