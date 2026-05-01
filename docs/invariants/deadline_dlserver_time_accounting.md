# Inactive DL Server Must Not Have Runtime Accounting Updated
**Source bug:** `c7f7e9c73178e0e342486fd31e7f363ef60e3f83`

**Property:** When the fair_server's `dl_server_active` flag is false (server is stopped), no scheduling path should modify the fair_server's runtime accounting fields (`runtime`, `dl_throttled`, `dl_yielded`).

**Variables:**
- `fs_runtime_before` — snapshot of `rq->fair_server.runtime` before `update_curr()` executes. Recorded at the entry of `update_curr()`. Read directly from `rq->fair_server.runtime`.
- `fs_active` — whether the fair_server is active. Recorded at the entry of `update_curr()`. Read via `dl_server_active(&rq->fair_server)` (or equivalently `rq->fair_server.dl_server_active`).
- `fs_runtime_after` — value of `rq->fair_server.runtime` after `update_curr()` completes. Read directly from `rq->fair_server.runtime`.
- `fs_throttled_after` — value of `rq->fair_server.dl_throttled` after `update_curr()` completes. Read directly.
- `fs_yielded_after` — value of `rq->fair_server.dl_yielded` after `update_curr()` completes. Read directly.

**Check(s):**

Check 1: Performed after `update_curr()` returns. Precondition: the fair_server was inactive at entry.
```c
// At entry of update_curr():
struct sched_dl_entity *fs = &rq->fair_server;
int was_active = dl_server_active(fs);
s64 saved_runtime = fs->runtime;
int saved_throttled = fs->dl_throttled;
int saved_yielded = fs->dl_yielded;

// ... update_curr() body executes ...

// At exit of update_curr():
if (!was_active) {
    WARN_ON_ONCE(fs->runtime != saved_runtime);
    WARN_ON_ONCE(fs->dl_throttled != saved_throttled);
    WARN_ON_ONCE(fs->dl_yielded != saved_yielded);
}
```

Check 2: Performed after `update_curr_task()` returns, when called from any scheduling class path (including RT/DL via `update_curr_common()`). Precondition: `p->dl_server` points to a dl_server entity.
```c
// Before update_curr_task():
struct sched_dl_entity *dl_se = p->dl_server;
s64 saved_runtime = dl_se ? dl_se->runtime : 0;

// After update_curr_task():
if (dl_se && !dl_server_active(dl_se)) {
    WARN_ON_ONCE(dl_se->runtime != saved_runtime);
}
```

**Example violation:** The buggy `update_curr_task()` calls `dl_server_update(p->dl_server, delta_exec)` when `p->dl_server` is non-NULL but points to a stopped fair_server (stopped during a delayed-dequeue pick path). This decrements `runtime` and sets `dl_throttled`/`dl_yielded` on the inactive server, violating the invariant.

**Other bugs caught:** `b53127db1dbf` (companion commit — dl_server double enqueue caused by same inactive-server accounting corruption)
