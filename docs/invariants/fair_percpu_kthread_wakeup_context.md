# Per-CPU Kthread Wakeup Context
**Source bug:** `8b4e74ccb582797f6f0b0a50372ebd9fd2372a27`

No generic invariant applicable. The bug is a missing context check (`in_task()`) on a specific optimization path in `select_idle_sibling()`; it is a logic error about when `current` can be trusted as the waker, not a violation of observable scheduler state consistency.
