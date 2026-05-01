# NUMA Placement No Node Crash
**Source bug:** `ab31c7fd2d37bc3580d9d712d5f2dfb69901fca9`

No generic invariant applicable. This is a missing sentinel-value guard (NUMA_NO_NODE == -1 passed as a bitmap index to node_state()), which is an ad-hoc API misuse / bounds-check omission rather than a violation of a reusable scheduler state property.
