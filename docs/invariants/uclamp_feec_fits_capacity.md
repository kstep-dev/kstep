# Uclamp feec fits_capacity
**Source bug:** `244226035a1f9b2b6c326e55ae5188fab4f428cb`

No generic invariant applicable. The bug is a logic error where the wrong capacity-check function (`fits_capacity()` with its 20% migration margin) was applied to uclamp-boosted utilization values in `find_energy_efficient_cpu()`; this is a "wrong function called" bug in a specific code path, not a violation of a checkable state invariant.
