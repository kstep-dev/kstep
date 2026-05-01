# avg_load Condition Invariant
**Source bug:** `6c8116c914b65be5e4d6f66d69c8142eb0648c22`

No generic invariant applicable. This is a one-off inverted conditional (`<` vs `==`/`>=`) guarding a computation — the bug is a simple logic typo specific to this code path, not a violation of a reusable scheduler property.
