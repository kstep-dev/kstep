# Wakeup Placement Must Not Decrease Vruntime
**Source bug:** `a53ce18cacb477dd0513c607f187d16f0fa96f71`

**Property:** After `place_entity(cfs_rq, se, 0)` (wakeup placement, non-initial), `se->vruntime` must be greater than or equal to its value before the call.

This follows from the semantics of `place_entity` for wakeups: it computes a floor (`min_vruntime - thresh`) and uses `max_vruntime(se->vruntime, floor)` to ensure the entity is not placed too far behind. The result should always be `>= se->vruntime` since `max_vruntime` returns the greater of its two arguments. The long-sleeper exception (which bypasses `max_vruntime`) should only trigger for entities that slept ~104 days — never for recently-active tasks.

**Variables:**
- `vruntime_before` — `se->vruntime` immediately before `place_entity()` is called. Recorded at `enqueue_entity()`, just before the `place_entity(cfs_rq, se, 0)` call. Read directly from `se->vruntime` and saved in a local or shadow variable.
- `vruntime_after` — `se->vruntime` immediately after `place_entity()` returns. Read directly from `se->vruntime` at `enqueue_entity()`.

**Check(s):**

Check 1: Performed at `enqueue_entity()`, immediately after `place_entity(cfs_rq, se, 0)` returns. Only when `flags & ENQUEUE_WAKEUP` (i.e., wakeup placement, not initial fork/exec placement).
```c
// In enqueue_entity(), around the place_entity call:
if (flags & ENQUEUE_WAKEUP) {
    u64 vruntime_before = se->vruntime;
    place_entity(cfs_rq, se, 0);
    // place_entity for wakeup should never decrease vruntime
    WARN_ON_ONCE((s64)(se->vruntime - vruntime_before) < 0);
}
```

**Example violation:** On the buggy kernel, `migrate_task_rq_fair()` clears `se->exec_start = 0` before `place_entity()` runs. This causes `entity_is_long_sleeper()` (or its predecessor) to falsely detect every migrated task as a long sleeper, bypassing `max_vruntime()` and unconditionally setting `se->vruntime = min_vruntime - thresh`. For any migrated task whose vruntime was ahead of the destination's `min_vruntime`, this decreases vruntime, violating the invariant.

**Other bugs caught:** Any future regression where long-sleeper detection is incorrectly triggered, or where `place_entity` otherwise resets vruntime downward for wakeups.
