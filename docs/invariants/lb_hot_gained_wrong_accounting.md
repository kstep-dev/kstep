# LB Hot Gained Wrong Accounting
**Source bug:** `a430d99e349026d53e2557b7b22bd2ebd61fe12a`

No generic invariant applicable. This is a premature-accounting bug where schedstat counters were incremented at the eligibility-check point (`can_migrate_task`) rather than the actual-migration point (`detach_task`); the violated property is "increment event counters only when the event occurs," which is a code-placement issue not expressible as a predicate over scheduler state.
