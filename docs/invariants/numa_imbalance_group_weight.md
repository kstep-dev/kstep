# NUMA Imbalance Parameter Consistency
**Source bug:** `2cfb7a1b031b0e816af7a6ee0c6ab83b0acdf05a`

No generic invariant applicable. The bug is a wrong-argument error (wrong group, wrong weight, missing +1) across multiple call sites of the same function — a code consistency issue with no single runtime state predicate that can distinguish correct from incorrect parameters without knowing the caller's semantic intent.
