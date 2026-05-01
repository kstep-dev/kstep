# SMT4 group_smt_balance Handling
**Source bug:** `450e749707bc1755f22b505d9cd942d4869dc535`

No generic invariant applicable. Two SMT4-specific logic errors: (1) a missing case-branch causing `group_smt_balance` with idle CPUs to use the wrong comparison metric (`avg_load`, always 0), and (2) an off-by-one in the `sibling_imbalance()` rounding guard (`== 0` should be `<= 1`). Both are narrow fixes to SMT4 code paths with no reusable property that generalizes beyond this exact scenario.
