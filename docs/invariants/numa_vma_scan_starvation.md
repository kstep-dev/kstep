# NUMA VMA Scan Starvation
**Source bug:** `f22cde4371f3c624e947a35b075c06c771442a43`

No generic invariant applicable. Bug is a NUMA-subsystem-specific feedback loop (cleared `pids_active` → no scan → no faults → no PID bits → no scan) involving VMA/mm state outside core scheduler structures; the invariant would only catch this exact starvation pattern and is not expressible over general scheduler state.
