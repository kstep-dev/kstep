# Forceidle Balance Wrong Context
**Source bug:** `5b6547ed97f4f5dfc23f8e3970af6d11d7b7ed7e`

No generic invariant applicable. The bug is about calling-context correctness (balance callback queued from `set_next_task_idle()` instead of only from `pick_next_task()` within `__schedule()`), which is a code-placement issue not expressible as a predicate over scheduler state.
