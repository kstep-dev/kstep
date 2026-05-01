# on_rq consistency after scheduling class switch
**Source bug:** `75b6499024a6c1a4ef0288f280534a5c54269076`

**Property:** After a scheduling class switch completes, if the task is sleeping (not TASK_RUNNING) and not queued in its new scheduling class, then `p->on_rq` must be 0.

**Variables:**
- `p->on_rq` — whether the task is logically queued on a runqueue. Read in-place at the check point. Value 0 means dequeued, TASK_ON_RQ_QUEUED (1) means queued.
- `p->__state` — the task's sleep/run state. Read in-place. TASK_RUNNING (0) means running/runnable; non-zero means sleeping.
- `task_on_rq_queued(p)` — macro checking `p->on_rq == TASK_ON_RQ_QUEUED`. Read in-place.
- `prev_class` — the scheduling class before the switch. Recorded by `check_class_changed()` caller (already available as a local variable in `__sched_setscheduler()`).
- `p->sched_class` — the new scheduling class after the switch. Read in-place.

**Check(s):**

Check 1: Performed at the end of `check_class_changed()` (after `switched_from_*` and `switched_to_*` have both been called). Precondition: the task's class actually changed (`prev_class != p->sched_class`).
```c
// After switched_from_*() and switched_to_*() complete:
if (prev_class != p->sched_class &&
    READ_ONCE(p->__state) != TASK_RUNNING &&
    !task_on_rq_queued(p)) {
	// Task is sleeping and was not re-enqueued into new class.
	// This path is taken when a sched_delayed task gets class-changed.
	SCHED_WARN_ON(p->on_rq != 0);
}
```

Check 2: Performed at the end of `switched_from_fair()`, specifically after the `sched_delayed` cleanup block. Precondition: `p->se.sched_delayed` was true on entry.
```c
// After the sched_delayed cleanup in switched_from_fair():
if (was_sched_delayed) {
	// The task was sleeping with delayed dequeue; after cleanup
	// it must be fully deactivated.
	SCHED_WARN_ON(p->on_rq != 0);
}
```

**Example violation:** In the buggy kernel, `switched_from_fair()` dequeues a `sched_delayed` task from its new class but never calls `__block_task()`, leaving `p->on_rq == TASK_ON_RQ_QUEUED` while the task is not on any runqueue. Check 1 fires because `__state != TASK_RUNNING`, the task is not queued in the new class, yet `on_rq != 0`.

**Other bugs caught:** Unknown — but this invariant would catch any future bug in any `switched_from_*()` handler that fails to properly finalize `p->on_rq` when cleaning up class-specific deferred state during a scheduling class transition.
