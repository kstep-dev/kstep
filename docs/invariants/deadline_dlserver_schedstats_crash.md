# dl_server schedstats crash
**Source bug:** `9c602adb799e72ee537c0c7ca7e828c3fe2acad6`

No generic invariant applicable. Bug is a missing type-guard (dl_server check) before calling `dl_task_of()` in the schedstats path — a one-off coding oversight when a new entity type was introduced, not a violation of a scheduler state invariant.
