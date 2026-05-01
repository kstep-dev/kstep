# Loadavg Accounting Race Invariant
**Source bug:** `dbfb089d360b1cc623c51a2c7cf9b99eff78e0e7`

No generic invariant applicable. This is a cross-CPU memory ordering race between `__schedule()` and `try_to_wake_up()` where `nr_uninterruptible` drift accumulates probabilistically over many sleep/wake cycles; no single scheduling event exhibits a checkable state violation, and verifying the global sum of `nr_uninterruptible` against actual blocked tasks requires walking all tasks (too expensive).
