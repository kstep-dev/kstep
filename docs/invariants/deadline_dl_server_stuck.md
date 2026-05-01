# Active dl_server Must Have a Liveness Mechanism
**Source bug:** `4ae8d9aa9f9dc7137ea5e564d79c5aa5af1bc45c`

**Property:** If a dl_server is marked active (`dl_server_active == 1`), it must either be enqueued on the DL runqueue, have a pending timer (`dl_timer` or defer timer active), or be currently running — otherwise the server is dead with no way to recover.

**Variables:**
- `dl_server_active` — whether the dl_server considers itself active. Read directly from `rq->fair_server.dl_server_active`.
- `on_dl_rq` — whether the dl_server's sched_dl_entity is enqueued on the DL runqueue. Recorded at check time. Obtained via `RB_EMPTY_NODE(&dl_se->rb_node)` being false, or `dl_rq->dl_nr_running > 0` on the relevant rq.
- `timer_pending` — whether the dl_timer hrtimer is armed. Recorded at check time. Obtained via `hrtimer_active(&dl_se->dl_timer)`.
- `dl_throttled` — whether the server is throttled (waiting for timer). Read directly from `dl_se->dl_throttled`.
- `is_current` — whether the dl_server entity is the currently executing DL entity. Checked via `dl_se == &rq->curr->dl` or by verifying the dl_server is the picked entity.

**Check(s):**

Check 1: Performed at `scheduler_tick` / `task_tick_dl` or a periodic audit point. Precondition: `dl_server_active == 1` on the rq's fair_server.
```c
struct sched_dl_entity *dl_se = &rq->fair_server;

if (dl_se->dl_server_active) {
    bool on_rq = !RB_EMPTY_NODE(&dl_se->rb_node);
    bool timer_active = hrtimer_active(&dl_se->dl_timer);
    bool is_running = (rq->dl.dl_nr_running > 0) || dl_se->dl_defer_running;

    // An active dl_server must have at least one liveness mechanism
    WARN_ON_ONCE(!on_rq && !timer_active && !is_running);
}
```

Check 2: Performed at exit of `dl_server_timer()` when returning `HRTIMER_NORESTART`. At this point the timer is being disarmed, so the server must be enqueued or another timer must be pending.
```c
// After dl_server_timer() returns HRTIMER_NORESTART:
struct sched_dl_entity *dl_se = /* the server entity */;

if (dl_se->dl_server_active) {
    bool on_rq = !RB_EMPTY_NODE(&dl_se->rb_node);
    // The timer we just handled is no longer pending, so
    // the server must be enqueued or must have been stopped
    WARN_ON_ONCE(!on_rq && !dl_se->dl_throttled);
}
```

**Example violation:** The buggy `dl_server_timer()` hits the `!server_has_tasks()` early exit, calls `replenish_dl_entity()` (which clears `dl_throttled` but does not arm the defer timer), then returns `HRTIMER_NORESTART`. The dl_server is left with `dl_server_active == 1`, not on the DL runqueue, no timer pending, and not running — violating the invariant. All recovery paths (e.g., `dl_server_start()`) fail because they see `dl_server_active` and return early.

**Other bugs caught:** Potentially `deadline_dlserver_timer_starvation` and `deadline_dl_server_stopped_inversion` if they involve similar dead-server states where the active flag and actual liveness mechanisms become inconsistent.
