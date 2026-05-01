# Deadline dl_boosted Uninitialized
**Source bug:** `ce9bc3b27f2a21a7969b41ffb04df8cf61bd1592`

No generic invariant applicable. This is a field initialization omission in `__dl_clear_params()` — a one-off coding oversight where `dl_boosted` was not zeroed alongside the other flags; the existing `WARN_ON(dl_se->dl_boosted)` in `setup_new_dl_entity()` already expressed the correct invariant, the bug was simply missing initialization code.
