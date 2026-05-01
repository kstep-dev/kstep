# No Generic Invariant for balance_switch Performance Regression
**Source bug:** `ae7927023243dcc7389b2d59b16c09cbbeaecc36`

No generic invariant applicable. This is a pure performance regression (extra memory load in context-switch hot path) with no scheduler state or correctness violation — all scheduling decisions remained correct.
