# task_call_func Race Invariant
**Source bug:** `91dabf33ae5df271da63e87ad7833e5fdb4a44b9`

No generic invariant applicable. This is a synchronization race — the state `{on_rq=0, sleeping, on_cpu=1}` is a valid transient state during `__schedule()`; the bug is that `task_call_func()` accesses the task in this state without proper synchronization (missing `smp_cond_load_acquire` on `on_cpu`), which is a one-off API-level locking fix rather than a checkable scheduler state invariant.
