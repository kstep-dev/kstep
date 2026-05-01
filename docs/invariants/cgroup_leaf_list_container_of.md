# Invalid container_of on leaf_cfs_rq_list Head
**Source bug:** `3b4035ddbfc8e4521f85569998a7569668cccf51`

No generic invariant applicable. This is a one-off list traversal boundary error — `container_of` was applied to a list sentinel head instead of an embedded node — a coding pattern mistake rather than a scheduler state invariant violation.
