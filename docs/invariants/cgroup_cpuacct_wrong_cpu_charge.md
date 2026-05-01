# cpuacct Wrong CPU Charge
**Source bug:** `248cc9993d1cc12b8e9ed716cc3fc09f6c3517dd`

No generic invariant applicable. This is a one-off implementation bug where `__this_cpu_add()` was used instead of `per_cpu_ptr(ca->cpuusage, task_cpu(tsk))` in cpuacct accounting — a wrong percpu access pattern specific to this function, not a general scheduler state property.
