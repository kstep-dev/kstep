# Uclamp Static Key Deadlock
**Source bug:** `e65855a52b479f98674998cb23b21ef5a8144b04`

No generic invariant applicable. This is a locking context violation (sleeping under spinlock), not a scheduler state consistency bug — no scheduler state predicate can detect it.
