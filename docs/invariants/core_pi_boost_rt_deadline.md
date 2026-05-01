# PI Boost RT-to-DL Type Guard
**Source bug:** `740797ce3a124b7dd22b7fb832d87bc8fba1cf6f`

No generic invariant applicable. The bug is a missing type-check (`dl_prio()`) before accessing `sched_dl_entity` fields on a non-DEADLINE task in `rt_mutex_setprio()`—a one-off precondition guard error in a specific PI code path, not a violation of a general scheduler state property.
