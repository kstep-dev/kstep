# Deadline Defer Bitwise AND Typo
**Source bug:** `22368fe1f9bbf39db2b5b52859589883273e80ce`

No generic invariant applicable. This is a purely cosmetic typo (`&` vs `&&`) with no observable behavioral difference — both operators produce identical results on 1-bit bitfields, so no runtime state violation exists to check.
