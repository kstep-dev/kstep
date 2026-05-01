# RT Throttled Implies Period Timer Active
**Source bug:** `9b58e976b3b391c0cf02e038d53dd0478ed3013c`

**Property:** If an RT runqueue is throttled, the corresponding RT bandwidth period timer must be active (so that throttling will eventually be resolved).

**Variables:**
- `rt_throttled` — whether the RT runqueue is currently throttled. Read in-place from `rt_rq->rt_throttled` at check time.
- `rt_period_active` — whether the RT bandwidth period timer is running. Read in-place from `sched_rt_bandwidth(rt_rq)->rt_period_active` at check time.

**Check(s):**

Check 1: Performed after `sched_rt_runtime_exceeded()` returns true in `update_curr_rt()`. When `rt_rq->rt_throttled` is set to 1.
```c
// After any code path sets rt_rq->rt_throttled = 1:
struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
if (rt_rq->rt_throttled && !rt_b->rt_period_active) {
    // VIOLATION: throttled with no timer to unthrottle
}
```

Check 2: Performed at `scheduler_tick` / `task_tick_rt`. Periodic consistency check across all RT runqueues on this CPU.
```c
struct rt_rq *rt_rq = &cpu_rq(cpu)->rt;
struct rt_bandwidth *rt_b = sched_rt_bandwidth(rt_rq);
if (rt_rq->rt_throttled && !rt_b->rt_period_active) {
    // VIOLATION: RT runqueue stuck throttled with inactive period timer
}
```

**Example violation:** When `sched_rt_runtime_us` is changed from `-1` to a finite value while an RT task is running, the task gets throttled in `update_curr_rt()` but the period timer was never started (it was skipped due to `RUNTIME_INF`). The invariant `rt_throttled → rt_period_active` fails, and the task is permanently starved.

**Other bugs caught:** None known, but this would catch any future code path that throttles an RT runqueue without ensuring the replenishment timer is armed (e.g., new sysctl handlers, cgroup RT bandwidth changes, or RT group scheduling paths).
