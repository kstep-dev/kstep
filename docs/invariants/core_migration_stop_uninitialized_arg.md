# Migration Pending Uninitialized Arg
**Source bug:** `8a6edb5257e2a84720fe78cb179eca58ba76126f`

No generic invariant applicable. This is an uninitialized struct field bug — `pending->arg` was never populated before use in a specific code path — which is a one-off coding error rather than a violation of a reusable scheduler state property.
