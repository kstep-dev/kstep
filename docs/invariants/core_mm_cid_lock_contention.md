# mm_cid Lock Contention Performance Regression
**Source bug:** `223baf9d17f25e2608dbdff7232c095c1e612268`

No generic invariant applicable. This is a pure performance regression (lock contention scalability) with no correctness violation — scheduling decisions are identical between buggy and fixed kernels; only wall-clock time in spinlock slow paths differs.
