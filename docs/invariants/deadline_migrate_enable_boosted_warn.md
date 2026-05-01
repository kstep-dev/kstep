# PI-Boosted DL Entity Parameters Preserved Across Enqueue
**Source bug:** `0664e2c311b9fa43b33e3e81429cd0c2d7f9c638`

**Property:** When a PI-boosted deadline scheduling entity is enqueued, its deadline and runtime parameters must not be reinitialized (they were set by the PI mechanism and must be preserved until deboosting).

**Variables:**
- `dl_se_deadline_before` — `dl_se->deadline` snapshot taken immediately before `enqueue_dl_entity()` executes its parameter-update branches. Recorded at entry of `enqueue_dl_entity()`, after the early-return path for already-on-rq entities. Saved in a local variable.
- `dl_se_runtime_before` — `dl_se->runtime` snapshot taken at the same point as above.
- `is_boosted` — `is_dl_boosted(dl_se)` evaluated at the same point. Read directly from `dl_se->pi_se != dl_se`.

**Check(s):**

Check 1: Performed at the end of `enqueue_dl_entity()`, after the ENQUEUE_WAKEUP / ENQUEUE_REPLENISH / ENQUEUE_RESTORE branches but before the throttle check. Only when the entity is PI-boosted at entry.
```c
// At entry to enqueue_dl_entity(), after early return for on_rq:
u64 saved_deadline = dl_se->deadline;
u64 saved_runtime = dl_se->runtime;
int was_boosted = is_dl_boosted(dl_se);

// ... existing ENQUEUE_WAKEUP / REPLENISH / RESTORE branches ...

// Check: boosted entity's parameters must be untouched
if (was_boosted) {
    WARN_ON_ONCE(dl_se->deadline != saved_deadline);
    WARN_ON_ONCE(dl_se->runtime != saved_runtime);
}
```

**Example violation:** The ENQUEUE_RESTORE branch calls `setup_new_dl_entity()` on a PI-boosted task whose deadline has expired, which calls `replenish_dl_new_period()` and overwrites `dl_se->deadline` and `dl_se->runtime` with freshly computed values, destroying the PI-inherited parameters.

**Other bugs caught:** Potentially `deadline_dl_boosted_uninit` (unplanned) if it involves similar parameter corruption of boosted DL entities.
