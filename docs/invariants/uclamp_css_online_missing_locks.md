# Uclamp CSS Online Missing Locks
**Source bug:** `93b73858701fd01de26a4a874eb95f9b7156fd4b`

No generic invariant applicable. This is a locking/synchronization bug (missing mutex + rcu_read_lock around a function call), not a state consistency violation checkable via scheduler data structure predicates.
