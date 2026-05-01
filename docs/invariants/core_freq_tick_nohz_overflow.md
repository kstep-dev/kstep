# Frequency Tick Nohz Overflow
**Source bug:** `7fb3ff22ad8772bbf0e3ce1ef3eb7b09f431807f`

No generic invariant applicable. Bug is an architecture-specific (x86 APERF/MPERF MSR) arithmetic overflow in a hardware counter delta after long tickless periods — no scheduler-internal state is violated; the fix is a conditional guard on an arch callback, not a scheduler state consistency property.
