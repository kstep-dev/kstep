# Tunable Scaling Write Bug
**Source bug:** `703066188f63d66cc6b9d678e5b5ef1213c5938e`

No generic invariant applicable. Bug is a string-handling error (missing null terminator + missing range check) in a debugfs write handler, not a scheduler state invariant violation.
