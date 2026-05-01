# EAS Spare Capacity Underflow
**Source bug:** `da0777d35f47892f359c3f73ea155870bb595700`

No generic invariant applicable. Bug is an unsigned arithmetic underflow in a local variable (`spare_cap = cpu_cap - util`), not a violation of persistent scheduler state; the fix is a safe-subtraction idiom (`lsub_positive`), which is a coding practice issue rather than a checkable scheduler invariant.
