# topology_numa_find_nth_null
**Source bug:** `5ebf512f335053a42482ebff91e46c6dc156bf8c`

No generic invariant applicable. The bug is a missing NULL-check on a `bsearch()` return value in a utility function—a one-off defensive programming error, not a violation of scheduler state consistency.
