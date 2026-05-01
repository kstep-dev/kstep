# RT Push Task Scheduling Class Precondition
**Source bug:** `49bef33e4b87b743495627a529029156c6e09530`

No generic invariant applicable. Race condition where a function precondition (find_lowest_rq requires an RT task) is violated due to rq->curr being demoted from RT to CFS between push work being queued and executing; the fix is a specific guard check, not a checkable scheduler state invariant.
