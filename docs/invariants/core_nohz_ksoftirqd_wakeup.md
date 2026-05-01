# No Generic Invariant for nohz ksoftirqd wakeup
**Source bug:** `e932c4ab38f072ce5894b2851fea8bc5754bb8e5`

No generic invariant applicable. Bug is a one-off API misuse (`raise_softirq_irqoff` vs `__raise_softirq_irqoff`) causing unnecessary ksoftirqd wakeups; no scheduler state invariant is violated—only a performance inefficiency in softirq delivery path selection.
