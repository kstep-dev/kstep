# Debug SD Flags kfree Offset
**Source bug:** `8d4d9c7b4333abccb3bf310d76ef7ea2edb9828f`

No generic invariant applicable. This is a C memory management bug (kfree of interior pointer) in the procfs debug interface, not a violation of any scheduler state invariant.
