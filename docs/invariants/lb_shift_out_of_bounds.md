# Shift-out-of-bounds in load_balance() detach_tasks()
**Source bug:** `39a2a6eb5c9b66ea7c8055026303b3aa681b49a5`

No generic invariant applicable. This is a C-language undefined behavior issue (shift exponent exceeding type width), not a violation of a scheduler state invariant; the fix is a defensive coding pattern (`shr_bound()` macro) rather than enforcement of a reusable scheduler property.
