# user_cpus_ptr reflects last successful sched_setaffinity
**Source bug:** `df14b7f9efcda35e59bb6f50351aac25c50f6e24`

**Property:** After `__set_cpus_allowed_ptr_locked()` returns success (ret == 0) with the `SCA_USER` flag set, `p->user_cpus_ptr` must be non-NULL and equal to the user-requested mask that was passed in via `ctx->user_mask`.

**Variables:**
- `user_mask_snapshot` — copy of the user-requested cpumask from `ctx->user_mask` before calling `__set_cpus_allowed_ptr_locked()`. Recorded at `__sched_setaffinity()` (or any SCA_USER caller) just before the call. Snapshot into a shadow variable since `ctx->user_mask` may be swapped/freed.
- `p->user_cpus_ptr` — the task's stored user-requested affinity mask. Read in-place from `task_struct` after the function returns successfully.
- `ctx->flags` — the affinity context flags. Read in-place; check for `SCA_USER` bit.
- `ret` — return value of `__set_cpus_allowed_ptr_locked()`. Read in-place after the call.

**Check(s):**

Check 1: Performed at `__set_cpus_allowed_ptr_locked()` return site (or wrapper `__sched_setaffinity()`). Precondition: `ctx->flags & SCA_USER` and `ret == 0`.
```c
// After __set_cpus_allowed_ptr_locked() returns:
if ((ctx->flags & SCA_USER) && ret == 0) {
    WARN_ON_ONCE(p->user_cpus_ptr == NULL);
    WARN_ON_ONCE(p->user_cpus_ptr &&
                 !cpumask_equal(p->user_cpus_ptr, &user_mask_snapshot));
}
```

**Example violation:** When `cpumask_equal(&p->cpus_mask, ctx->new_mask)` is true, the early `goto out` returns success without performing `swap(p->user_cpus_ptr, ctx->user_mask)`, leaving `user_cpus_ptr` NULL or stale despite a successful SCA_USER call.

**Other bugs caught:** Potentially `core_setaffinity_nonsmp_null` (NULL user_cpus_ptr on non-SMP), `core_dup_user_cpus_uaf` (user_cpus_ptr mismanagement on fork), and any future bug where a new early-exit or code path in the affinity-setting chain skips the user_cpus_ptr update.
