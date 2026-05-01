# Redundant Variable and Sparse Warning in build_sched_domains
**Source bug:** `7f434dff76215af00c26ba6449eaa4738fe9e2ab`

No generic invariant applicable. This is a compile-time code quality fix (redundant variable removal and `__rcu` type annotation correction) with no runtime behavioral difference between buggy and fixed kernels.
