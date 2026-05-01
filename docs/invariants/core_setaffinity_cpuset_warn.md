# core_setaffinity_cpuset_warn
**Source bug:** `70ee7947a29029736a1a06c73a48ff37674a851b`

No generic invariant applicable. The bug is a spurious `WARN_ON_ONCE` on a legitimate race outcome — no scheduler state invariant is violated; the functional behavior (fallback to cpuset mask) is correct in both buggy and fixed kernels.
