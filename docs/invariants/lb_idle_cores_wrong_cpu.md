# has_idle_cores Wrong CPU
**Source bug:** `02dbb7246c5bbbbe1607ebdc546ba5c454a664b1`

No generic invariant applicable. This is a one-off wrong-variable bug (`this` vs `target` in a single `set_idle_cores()` call) — a copy-paste error from a refactor, not a violation of a broadly reusable scheduler property.
