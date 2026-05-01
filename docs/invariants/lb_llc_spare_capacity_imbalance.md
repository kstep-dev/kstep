# LLC Spare Capacity Imbalance
**Source bug:** `16b0a7a1a0af9db6e008fecd195fe4d6cb366d83`

No generic invariant applicable. The bug is a policy error (wrong migration strategy selected for LLC-level domains); the fix adds a domain-flag check (`SD_SHARE_PKG_RESOURCES`) to a specific branch in `calculate_imbalance()`, which is an ad-hoc heuristic choice rather than a violable structural property of scheduler state.
