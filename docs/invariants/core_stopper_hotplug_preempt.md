# stop_one_cpu_nowait() vs Hotplug Preemption Race
**Source bug:** `f0498d2a54e7966ce23cd7c7ff42c64fa0059b07`

No generic invariant applicable. This is a preemption race between rq-lock release and `stop_one_cpu_nowait()` — no scheduler state is corrupted; the stopper silently fails to queue, which is outside scheduler-observable state.
