# util_est Stale During Dequeue
**Source bug:** `8c1f560c1ea3f19e22ba356f62680d9d449c9ec2`

No generic invariant applicable. The bug is a mid-function ordering issue (util_est decremented after schedutil notification rather than before); the state is correct at function entry/exit boundaries, so no point or consistency invariant over scheduler structs can catch it without hooking into the internal notification callchain.
