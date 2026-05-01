# Cpufreq Tunables Kobject Premature Free
**Source bug:** `e5c6b312ce3cc97e90ea159446e6bfa06645364d`

No generic invariant applicable. This is a kobject lifecycle violation (premature kfree bypassing kobject release callback) in cpufreq governor infrastructure, not a scheduler state invariant — no rq/cfs_rq/task_struct fields are involved.
