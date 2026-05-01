# housekeeping_any_cpu Boot Crash
**Source bug:** `5097cbcb38e6e0d2627c9dde1985e91d2c9f880e`

No generic invariant applicable. This is a boot-time API contract violation (missing return-value validation on `cpumask_any_and()` in `housekeeping_any_cpu()`), not a scheduler runtime state invariant — it only manifests before `smp_init()` and cannot be observed from scheduler structs (rq, cfs_rq, etc.) during normal operation.
