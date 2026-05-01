# Selected CPU Must Be In Task's Allowed Affinity Mask
**Source bug:** `ae2ad293d6be143ad223f5f947cca07bcbe42595`

**Property:** The CPU returned by `select_task_rq()` for a task must be set in that task's `p->cpus_ptr` affinity mask.

**Variables:**
- `selected_cpu` — the CPU chosen for task placement. Recorded at return of `select_task_rq_fair()` (or more broadly, at `select_task_rq()` dispatch). Read directly from the return value.
- `p->cpus_ptr` — the task's current allowed CPU mask. Read in-place from `task_struct` at the same point.

**Check(s):**

Check 1: Performed at the return site of `select_task_rq_fair()` (or equivalently, after `select_task_rq()` returns in `try_to_wake_up`). No preconditions — applies to every task placement.
```c
int cpu = select_task_rq(p, ...);
// Invariant: selected CPU must be in the task's affinity mask
WARN_ON_ONCE(!cpumask_test_cpu(cpu, p->cpus_ptr));
```

Check 2: Performed at `ttwu_do_activate()` or `__set_task_cpu()` as a secondary safety net, after the CPU has been committed.
```c
// In __set_task_cpu(p, new_cpu):
WARN_ON_ONCE(!cpumask_test_cpu(new_cpu, p->cpus_ptr));
```

**Example violation:** The bug overwrites `p->recent_used_cpu = prev` before the affinity check, then tests `p->recent_used_cpu` (now `prev`) instead of the local `recent_used_cpu` variable. When `prev` is in `cpus_ptr` but `recent_used_cpu` is not, the function returns a CPU outside the task's allowed mask.

**Other bugs caught:** Any bug in `select_idle_sibling`, `select_idle_cpu`, `select_idle_core`, or wake-affine logic that returns a CPU not in the task's affinity mask — a broad class of task-placement correctness bugs.
