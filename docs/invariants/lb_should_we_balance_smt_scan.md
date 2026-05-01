# SMT Sibling Redundant Scan
**Source bug:** `f8858d96061f5942216c6abb0194c3ea7b78e1e8`

No generic invariant applicable. This is a pure performance optimization (redundant iteration over SMT siblings in `should_we_balance()`), not a correctness bug — the buggy and fixed kernels produce identical scheduling decisions with no observable state violation.
