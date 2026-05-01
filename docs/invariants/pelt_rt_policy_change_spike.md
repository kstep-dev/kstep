# PELT per-rq last_update_time must be current at scheduling class transitions
**Source bug:** `fecfcbc288e9f4923f40fd23ca78a6acdc7fdf6c`

**Property:** When a running task switches scheduling class (via `switched_to_rt`, `switched_to_dl`, etc.), the target class's per-rq PELT `sched_avg.last_update_time` must be synchronized to the current `rq_clock_pelt(rq)` before any running time is accumulated under the new class.

**Variables:**
- `class_avg_last_update` — `last_update_time` field of the per-rq PELT average for the target scheduling class (`rq->avg_rt.last_update_time` for RT, `rq->avg_dl.last_update_time` for DL). Read at the `switched_to_*` hook, after the hook completes.
- `rq_pelt_clock` — current PELT clock for the runqueue, obtained via `rq_clock_pelt(rq)`. Read at the same point.

**Check(s):**

Check 1: Performed at `switched_to_rt()` exit, when `task_current(rq, p)` (i.e., the switched task is currently running).
```c
// After switched_to_rt() completes for rq->curr:
u64 delta = rq_clock_pelt(rq) - rq->avg_rt.last_update_time;
// last_update_time should have been synced; delta should be
// at most one PELT period (1024us = 1ms). A stale timestamp
// would show a delta of many milliseconds.
WARN_ON_ONCE(delta > 2 * PELT_PERIOD);  // ~2ms tolerance
```

Check 2: Performed at `switched_to_dl()` exit, when `task_current(rq, p)`.
```c
u64 delta = rq_clock_pelt(rq) - rq->avg_dl.last_update_time;
WARN_ON_ONCE(delta > 2 * PELT_PERIOD);
```

**Example violation:** Before the fix, `switched_to_rt()` skipped the body entirely for `rq->curr == p`, leaving `avg_rt.last_update_time` potentially seconds stale. The subsequent `put_prev_task_rt()` computed a massive delta, spiking `avg_rt.util_avg` to near 1024.

**Other bugs caught:** Could catch analogous missing PELT sync in `switched_to_dl()` or any future scheduling class that adds per-rq PELT tracking without handling the running-task policy-change path.
