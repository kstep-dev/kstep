# Topology hop_cmp OOB Read
**Source bug:** `01bb11ad828b320749764fa93ad078db20d08a9e`

No generic invariant applicable. This is a memory safety bug (out-of-bounds array dereference in a bsearch comparator), not a scheduler state invariant violation — the function produces correct scheduling results on both buggy and fixed kernels.
