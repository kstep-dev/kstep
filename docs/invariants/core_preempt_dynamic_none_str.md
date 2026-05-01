# Preempt Dynamic None String
**Source bug:** `3ebb1b6522392f64902b4e96954e35927354aa27`

No generic invariant applicable. Bug is an off-by-one in a string-formatting comparison (`> 0` vs `>= 0`) with no scheduler state violated — purely cosmetic misreporting of the preemption model name.
