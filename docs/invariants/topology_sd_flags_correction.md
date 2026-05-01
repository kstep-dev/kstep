# Topology SD_flags Correction
**Source bug:** `9b1b234bb86bcdcdb142e900d39b599185465dbb`

No generic invariant applicable. This is a one-character bitwise operator typo (`&= ~X` vs `&= X`) in an error-correction path; the check detecting the violation was correct but the remediation inverted the mask — a one-off logic error with no reusable invariant.
