# cputime_adjust mul_u64_u64_div_u64 Overflow
**Source bug:** `77baa5bafcbe1b2a15ef9c37232c21279c95481c`

No generic invariant applicable. Architecture-specific arithmetic precision bug in a math utility function (`mul_u64_u64_div_u64` approximation on non-x86), not a scheduler state invariant violation.
