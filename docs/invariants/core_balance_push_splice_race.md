# balance_push splice race
**Source bug:** `04193d590b390ec7a0592630f46d559ec6564ba1`

No generic invariant applicable. Bug is a race condition in the CPU hotplug balance_push_callback mechanism where splice_balance_callbacks() temporarily removes a persistent callback across a lock-break window; the violated property is specific to this callback infrastructure's interaction with hotplug and not expressible as a reusable scheduler state predicate.
