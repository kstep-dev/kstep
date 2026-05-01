# Uclamp Fast Path Overhead
**Source bug:** `46609ce227039fd192e0ecc7d940bed587fd2c78`

No generic invariant applicable. This is a pure performance optimization (adding a static-key bypass to skip uclamp logic when unused), not a correctness bug — all scheduler state and decisions were correct, just computed unnecessarily.
