# Bandwidth !SMP Leaf List Assert
**Source bug:** `c0490bc9bb62d9376f3dd4ec28e03ca0fef97152`

No generic invariant applicable. Bug exists exclusively in the `CONFIG_SMP=n` stub of `cfs_rq_is_decayed()`, which unconditionally returned `true`; on SMP kernels (the only configuration testable by kSTEP), the function correctly checks `load.weight` and other fields, so this property is never violated.
