# Redundant Migration Check
**Source bug:** `3f1bc119cd7fc987c8ed25ffb717f99403bb308c`

No generic invariant applicable. This is a missing performance optimization (short-circuit check) in a specific migration path, not a scheduler state consistency violation — the task always ends up on a valid CPU either way.
