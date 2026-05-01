# TASK_state Bitmask Comparisons
**Source bug:** `5aec788aeb8eb74282b75ac1b317beb0fbb69a42`

No generic invariant applicable. Bug is a coding pattern error (using `==` instead of `&` on bitmask values) — not a violation of runtime scheduler state consistency; the task states themselves are correct, only the comparison operators reading them are wrong.
