# sched_feat Static Copy Invariant
**Source bug:** `a73f863af4ce9730795eab7097fb2102e6854365`

No generic invariant applicable. This is a compile-time preprocessor guard bug causing per-translation-unit static copies of a variable instead of a shared extern; no runtime scheduler state predicate can detect incorrect C linkage.
