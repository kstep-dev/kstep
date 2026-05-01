# Fair Reweight Entity NULL cfs_rq
**Source bug:** `13765de8148f71fa795e0a6607de37c49ea5915a`

No generic invariant applicable. This is a race condition during fork where a TASK_NEW task is transiently visible to thread-group iterators before scheduler state is initialized; the fix is a defensive TASK_NEW check specific to this fork-path timing window, not a general scheduler state property that can be checked at hook points.
