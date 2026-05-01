# Uclamp RQ Init Size Mismatch
**Source bug:** `dcd6dffb0a75741471297724640733fa4e958d72`

No generic invariant applicable. This is a one-off memset size typo in a boot-time `__init` function — a coding mistake with no reusable scheduler state property to check at runtime.
