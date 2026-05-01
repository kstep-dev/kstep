# Uclamp iowait boost bypass
**Source bug:** `d37aee9018e68b0d356195caefbb651910e0bbfa`

No generic invariant applicable. Bug is a missing `uclamp_rq_util_with()` call inside a cpufreq governor-private code path (`sugov_iowait_apply`); the affected state (`sg_cpu->util`) is internal to schedutil and not observable at standard scheduler hook points.
