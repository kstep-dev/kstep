# Load Balancer Must Not Pull Load to Above-Average Groups
**Source bug:** `91dcf1e8068e9a8823e419a7a34ff4341275fb70`

**Property:** When the load balancer computes a `migrate_load` imbalance, the local (destination) group's average load must be below the system-wide average load; otherwise the imbalance must be zero.

**Variables:**
- `local_avg_load` — the local scheduling group's average load (`group_load * SCHED_CAPACITY_SCALE / group_capacity`). Recorded at `calculate_imbalance()` after the local avg_load computation. Read from `local->avg_load` in `struct sg_lb_stats`.
- `sds_avg_load` — the system-wide average load (`total_load * SCHED_CAPACITY_SCALE / total_capacity`). Recorded at `calculate_imbalance()` after the sds avg_load computation. Read from `sds->avg_load` in `struct sd_lb_stats`.
- `imbalance` — the computed load to migrate. Read from `env->imbalance` in `struct lb_env` after `calculate_imbalance()` returns.
- `migration_type` — the type of migration selected. Read from `env->migration_type` in `struct lb_env`.
- `local_group_type` — classification of the local scheduling group. Read from `local->group_type` in `struct sg_lb_stats`.

**Check(s):**

Check 1: Performed at the end of `calculate_imbalance()`, when `migration_type == migrate_load` and `local->group_type < group_overloaded` (i.e., the local group is `group_fully_busy` or lower).
```c
// After calculate_imbalance() completes:
if (env->migration_type == migrate_load &&
    local->group_type < group_overloaded &&
    local->avg_load >= sds->avg_load) {
	// The local group is already at or above the system average.
	// Pulling more load would increase system imbalance.
	WARN_ON_ONCE(env->imbalance != 0);
}
```

Check 2: Performed at `sched_balance_rq()` / `load_balance()` after `calculate_imbalance()` returns, as a sanity bound on the imbalance magnitude.
```c
// After calculate_imbalance() returns with migrate_load:
if (env->migration_type == migrate_load && env->imbalance > 0) {
	// The imbalance should never exceed the busiest group's total load.
	// An unsigned underflow in the formula produces values near ULONG_MAX
	// which will vastly exceed any real group load.
	unsigned long busiest_load = busiest->group_load;
	WARN_ON_ONCE(env->imbalance > busiest_load);
}
```

**Example violation:** When `local->avg_load = 104` and `sds->avg_load = 103`, the unsigned subtraction `(103 - 104)` wraps to `ULONG_MAX`. The `min()` with the other operand then produces a non-zero imbalance (reported as 133), even though the local group is already above average and should have `imbalance = 0`. Check 1 fires because `imbalance != 0` while `local->avg_load >= sds->avg_load`.

**Other bugs caught:** None confirmed, but this invariant would catch any future arithmetic error in `calculate_imbalance()` that fails to guard unsigned subtractions in the `migrate_load` path, or any logic error that directs load toward an already-above-average group.
