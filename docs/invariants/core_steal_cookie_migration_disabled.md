# Migration-Disabled Tasks Must Not Change CPU
**Source bug:** `386ef214c3c6ab111d05e1790e79475363abaa05`

**Property:** If a task has `migration_disabled > 0`, `set_task_cpu()` must not change its CPU to a different one.

**Variables:**
- `p->migration_disabled` — migration disable nesting count. Read in-place at `set_task_cpu()`. Non-zero means the task must stay on its current CPU.
- `task_cpu(p)` — the task's current CPU before `set_task_cpu()`. Read in-place from `p->cpu` at the start of `set_task_cpu()`.
- `new_cpu` — the destination CPU argument to `set_task_cpu()`. Passed as a parameter.

**Check(s):**

Check 1: Performed at `set_task_cpu()`, before actually updating the task's CPU. No preconditions.
```c
// In set_task_cpu(struct task_struct *p, unsigned int new_cpu):
if (is_migration_disabled(p) && task_cpu(p) != new_cpu) {
    // VIOLATION: migrating a migration-disabled task to a different CPU
    WARN_ONCE(1, "set_task_cpu: task %s/%d migrated from %d to %d with migration_disabled=%d",
              p->comm, p->pid, task_cpu(p), new_cpu, p->migration_disabled);
}
```

**Example violation:** `try_steal_cookie()` only checked `cpumask_test_cpu()` before calling `set_task_cpu()`, missing that the task had `migration_disabled > 0`. This moved a migration-disabled task to a different CPU, breaking per-CPU data invariants.

**Other bugs caught:** Any future code path that directly calls `set_task_cpu()` without checking `migration_disabled` (e.g., new balancers, migration optimizations, or task stealing paths).
