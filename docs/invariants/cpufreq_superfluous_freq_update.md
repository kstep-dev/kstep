# Cpufreq Superfluous Freq Update
**Source bug:** `8e461a1cb43d69d2fc8a97e61916dce571e6bb31`

No generic invariant applicable. This is a cpufreq governor-internal flag management logic error (`need_freq_update` permanently latched true due to re-testing a static driver flag instead of unconditionally clearing), not a violation of a scheduler state invariant expressible over core scheduler structures (rq, cfs_rq, sched_entity, etc.).
