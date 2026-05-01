# Preempt Dynamic Return Value
**Source bug:** `9ed20bafc85806ca6c97c9128cec46c3ef80ae86`

No generic invariant applicable. Bug is a swapped return value convention (0/1) in a boot-time `__setup()` callback with no effect on any runtime scheduler state.
