# No Migration of Migration-Disabled Tasks
**Source bug:** `feffe5bb274dd3442080ef0e4053746091878799`

No generic invariant applicable. The kernel already enforces this property via `WARN_ON_ONCE(is_migration_disabled(p))` in `set_task_cpu()`; the bug is a TOCTOU race (missing revalidation after lock release) rather than an unmonitored state invariant.
