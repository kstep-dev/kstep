# Cpufreq Need Update Limits Bypass
**Source bug:** `cfde542df7dd51d26cf667f4af497878ddffd85a`

No generic invariant applicable. Bug is a cpufreq schedutil governor logic error (misplaced driver flag check) outside core scheduler state; not expressible as a predicate over rq/cfs_rq/task_struct structures.
