# dl_server Runtime Uses Wall-Clock Time
**Source bug:** `fc975cfb36393db1db517fbbe366e550bcdcff14`

**Property:** For dl_server entities, the runtime deducted from `dl_se->runtime` in any accounting path must equal the raw wall-clock `delta_exec`, not frequency/capacity-scaled time.

**Variables:**
- `dl_se->runtime_before` — dl_server's runtime before the accounting operation. Snapshot taken at entry to `update_curr_dl_se()` (or `dl_server_update_idle_time()`). Read directly from `dl_se->runtime`.
- `delta_exec` — the raw wall-clock execution delta passed into the accounting function. Read from the `delta_exec` parameter.
- `dl_se->runtime_after` — dl_server's runtime after the accounting operation. Read directly from `dl_se->runtime` after the subtraction.

**Check(s):**

Check 1: Performed at `update_curr_dl_se()`, after `dl_se->runtime -= scaled_delta_exec`. Only when `dl_server(dl_se)` is true, `delta_exec > 0`, the entity is not special, and the server is not in the early-return throttled+!defer state.
```c
// After dl_se->runtime subtraction:
if (dl_server(dl_se)) {
    s64 actual_deducted = runtime_before - dl_se->runtime;
    WARN_ON_ONCE(actual_deducted != delta_exec);
}
```

Check 2: Performed at `dl_server_update_idle_time()`, after `rq->fair_server.runtime -= delta_exec`. Only when `delta_exec >= 0` and `fair_server.runtime` was non-negative before the subtraction.
```c
// After fair_server.runtime subtraction (before the clamp to 0):
s64 actual_deducted = runtime_before - rq->fair_server.runtime;
WARN_ON_ONCE(actual_deducted != delta_exec);
```

**Example violation:** The buggy code calls `dl_scaled_delta_exec()` for dl_server entities, which scales `delta_exec` by `freq_scale * cap_scale >> 20`. On a LITTLE CPU with freq=100, cap=50, a 50ms wall-clock delta is scaled to ~238μs, so the invariant `actual_deducted == delta_exec` fails by a factor of ~209x.

**Other bugs caught:** None known, but this invariant guards against re-introduction of the same mistake in any code path that deducts runtime from a dl_server entity (the bug originally existed in two independent functions).
