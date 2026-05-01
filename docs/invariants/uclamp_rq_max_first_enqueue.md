# Uclamp rq aggregation consistency with enqueued tasks
**Source bug:** `315c4f884800c45cb6bd8c90422fad554a8b9588`

**Property:** When a runqueue transitions from idle (0 tasks) to active (1 task) via enqueue, `rq->uclamp[clamp_id].value` must equal the enqueued task's effective uclamp value for each clamp_id.

**Variables:**
- `rq_uclamp_val` — `rq->uclamp[clamp_id].value` after `uclamp_rq_inc()` completes. Read directly from the rq struct at the check point.
- `task_eff_uclamp` — the task's effective uclamp value `uclamp_eff_value(p, clamp_id)` for each clamp_id. Computed from the task's `uclamp_req` and cgroup restrictions at enqueue time.
- `prev_nr_running` — `rq->nr_running` before the enqueue operation. Snapshot taken at entry to `enqueue_task()` (or equivalently, check that `rq->nr_running == 1` after enqueue to confirm this was a 0→1 transition).

**Check(s):**

Check 1: Performed at the end of `enqueue_task()` (after `uclamp_rq_inc()` returns). Precondition: `rq->nr_running == 1` (the rq just transitioned from empty to having one task) and `static_branch_unlikely(&sched_uclamp_used)` is true.
```c
// After enqueue_task() completes for task p on rq:
if (rq->nr_running == 1 && static_branch_unlikely(&sched_uclamp_used)) {
    enum uclamp_id clamp_id;
    for_each_clamp_id(clamp_id) {
        unsigned int rq_val = READ_ONCE(rq->uclamp[clamp_id].value);
        unsigned int task_val = uclamp_eff_value(p, clamp_id);
        WARN_ON_ONCE(rq_val != task_val);
    }
}
```

**Example violation:** On a freshly booted kernel, `rq->uclamp_flags` is initialized to 0 (missing `UCLAMP_FLAG_IDLE`). When the first task with `uclamp_max=512` enqueues, `uclamp_idle_reset()` skips the reset (no IDLE flag), and the max-aggregation `512 > 1024` fails, leaving `rq->uclamp[UCLAMP_MAX].value` at 1024 instead of the task's 512.

**Other bugs caught:** `d81ae8aac85c` (rq uclamp_max initialized to 0 instead of 1024 — the 0→1 transition check would catch cases where the initial value leaks through); `ca4984a7dd86` (stale `UCLAMP_FLAG_IDLE` after active update could cause incorrect idle reset on next 0→1 enqueue, producing wrong rq uclamp values).
