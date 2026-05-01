# CFS Lone-Entity No Self-Preempt
**Source bug:** `d4ac164bde7a12ec0a238a7ead5aa26819bbb1c1`

**Property:** When a cfs_rq has exactly one runnable entity, CFS-internal preemption logic in `update_curr()` must not request a reschedule.

**Variables:**
- `cfs_rq->nr_running` — number of CFS entities on the current task's cfs_rq. Read in-place at the end of `update_curr()`.
- `TIF_NEED_RESCHED before` — whether TIF_NEED_RESCHED was already set before `update_curr()` runs. Snapshot taken at the entry of `update_curr()`.
- `TIF_NEED_RESCHED after` — whether TIF_NEED_RESCHED is set after `update_curr()` completes. Read in-place at the exit of `update_curr()`.

**Check(s):**

Check 1: Performed at the exit of `update_curr()`. Only when `cfs_rq->nr_running == 1` and the entity is the current running entity.
```c
// At entry of update_curr():
bool resched_was_set = test_tsk_need_resched(rq_curr(rq));

// At exit of update_curr(), after the preemption decision block:
if (cfs_rq->nr_running == 1 && !resched_was_set) {
    WARN_ON_ONCE(test_tsk_need_resched(rq_curr(rq)));
}
```

**Example violation:** The buggy code checked `rq->nr_running == 1` (all scheduling classes) instead of `cfs_rq->nr_running == 1`. When an RT task or a CFS task in a different cgroup hierarchy level was also runnable, `rq->nr_running > 1` caused the guard to fail, and `resched_curr()` was called despite the current task being the sole entity in its cfs_rq.

**Other bugs caught:** None known.
