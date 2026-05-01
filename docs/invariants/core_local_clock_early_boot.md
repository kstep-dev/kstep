# local_clock early boot guard
**Source bug:** `f31dcb152a3d0816e2f1deab4e64572336da197d`

No generic invariant applicable. Bug is a missing early-boot guard check in a refactored function (one-off code duplication omission in sched_clock subsystem init path); no reusable scheduler state predicate applies.
