# Cgroup Fork Stale Task Group
**Source bug:** `4ef0c5c6b5ba1f38f0ea1cedad0cad722f00c14a`

No generic invariant applicable. This is a race condition (fork vs. cgroup migration) where the stale `sched_task_group` pointer is only invalid during the narrow window between `dup_task_struct()` and `sched_fork()` inside `copy_process()` — before the task reaches any standard scheduler hook point — and is corrected by `cgroup_post_fork()` before the task is ever enqueued, making the violation unobservable at normal check points.
