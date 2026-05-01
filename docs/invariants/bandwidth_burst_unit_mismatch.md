# Bandwidth Burst Unit Mismatch
**Source bug:** `49217ea147df7647cb89161b805c797487783fc0`

No generic invariant applicable. This is a unit conversion error in a single cgroup write path (`cpu_max_write()` passes microseconds where nanoseconds are expected); it is not a violation of a general scheduler state property checkable at runtime hook points.
