# Fork Race Incomplete Init
**Source bug:** `b1e8206582f9d680cff7d04828708c8b6ab32957`

No generic invariant applicable. This is a process-lifecycle ordering bug (task visible via PID hash before scheduler init completes) — the violated property is about ordering between `attach_pid()` and `sched_cgroup_fork()` in `copy_process()`, which is not observable as a scheduler state predicate at any hook point.
