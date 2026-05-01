# nr_running Tracepoint Sign
**Source bug:** `a1bd06853ee478d37fae9435c5521e301de94c67`

No generic invariant applicable. Bug is a missing negation in a tracepoint argument — a one-off typo with no impact on scheduler state, only on tracing output.
