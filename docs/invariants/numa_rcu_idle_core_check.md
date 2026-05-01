# NUMA RCU Idle Core Check
**Source bug:** `0621df315402dd7bc56f7272fae9778701289825`

No generic invariant applicable. This is an RCU locking correctness bug (missing rcu_read_lock around sd_llc_shared access), not a scheduler state invariant violation; it is already caught by CONFIG_PROVE_RCU lockdep checks.
