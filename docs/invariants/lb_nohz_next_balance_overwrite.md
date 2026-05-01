# NOHZ next_balance Overwrite
**Source bug:** `3ea2f097b17e13a8280f1f9386c331b326a3dbef`

No generic invariant applicable. The bug is an operation-ordering and abort-handling issue specific to `_nohz_idle_balance()` — the meaningful invariant (`nohz.next_balance <= min(rq->next_balance)` across all NOHZ-idle CPUs) requires iterating all idle CPUs to check and is too expensive for a runtime check.
