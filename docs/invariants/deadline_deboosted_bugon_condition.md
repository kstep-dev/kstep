# Deadline Deboosted BUG_ON Condition
**Source bug:** `ddfc710395cccc61247348df9eb18ea50321cbed`

No generic invariant applicable. The bug is a logically contradictory BUG_ON assertion (always true when reached) — an incorrect defensive check, not a violation of scheduler state consistency.
