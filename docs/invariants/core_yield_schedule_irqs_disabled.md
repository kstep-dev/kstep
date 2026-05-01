# Interrupts Enabled on schedule() Entry
**Source bug:** `345a957fcc95630bf5535d7668a59ed983eb49a7`

No generic invariant applicable. This is a lock/unlock pairing mistake (`rq_unlock` vs `rq_unlock_irq`) — the invariant "interrupts must be enabled when entering `schedule()`" is already enforced by the kernel's `schedule_debug()` with `CONFIG_DEBUG_ATOMIC_SLEEP`, and is not a scheduler data-structure consistency property amenable to runqueue/entity-level checking.
