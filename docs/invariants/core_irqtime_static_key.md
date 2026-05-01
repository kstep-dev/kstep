# IRQ Time Static Key Sleeping in Atomic
**Source bug:** `f3fa0e40df175acd60b71036b9a1fd62310aec03`

No generic invariant applicable. Bug is an API-misuse issue (sleeping static_key operation called from atomic context), not a scheduler state consistency violation.
