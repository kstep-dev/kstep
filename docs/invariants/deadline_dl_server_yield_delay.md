# DL Server Must Not Yield
**Source bug:** `a3a70caf7906708bf9bbc80018752a6b36543808`

**Property:** A dl_server entity (`dl_server == 1`) should never have `dl_yielded == 1`, because yielding (zeroing runtime and deferring to the next period) is semantically incorrect for bandwidth servers — the correct action when a server has no work is to stop, not yield.

**Variables:**
- `dl_se->dl_server` — whether the entity is a dl_server. Read directly from `struct sched_dl_entity`.
- `dl_se->dl_yielded` — whether the entity has been marked as yielded. Read directly from `struct sched_dl_entity`.

**Check(s):**

Check 1: Performed at `update_curr_dl_se()`, before processing the yield flag. Always checked when the entity is a dl_server.
```c
if (dl_server(dl_se)) {
    WARN_ON_ONCE(dl_se->dl_yielded);
}
```

Check 2: Performed at `__pick_task_dl()`, after the `server_pick_task()` call returns (whether NULL or not). Always checked for dl_server entities.
```c
if (dl_server(dl_se)) {
    WARN_ON_ONCE(dl_se->dl_yielded);
}
```

**Example violation:** The buggy code in `__pick_task_dl()` sets `dl_se->dl_yielded = 1` on the dl_server when `server_pick_task()` returns NULL, causing `update_curr_dl_se()` to zero the server's runtime via `dl_yield()`. This defers the server's next scheduling opportunity by an entire period (1 second), starving CFS tasks.

**Other bugs caught:** None known, but this invariant would catch any future regression that incorrectly applies the yield mechanism to dl_server entities instead of stopping them.
