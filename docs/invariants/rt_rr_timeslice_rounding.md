# RT RR Timeslice Rounding
**Source bug:** `c7fcb99877f9f542c918509b2801065adcaf46fa`

No generic invariant applicable. This is a compile-time integer arithmetic rounding error in a static initializer (`(MSEC_PER_SEC / HZ) * RR_TIMESLICE` vs `(MSEC_PER_SEC * RR_TIMESLICE) / HZ`), not a runtime scheduler state violation — it only affects one variable's initial value under `CONFIG_HZ_300` and has no generalizable runtime predicate.
