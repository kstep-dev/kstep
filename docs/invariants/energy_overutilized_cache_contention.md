# EAS Overutilized Cache Contention
**Source bug:** `be3a51e68f2f1b17250ce40d8872c7645b7a2991`

No generic invariant applicable. Pure performance bug (cache contention from unnecessary writes to `rd->overutilized` on non-EAS systems) with no observable scheduler state violation — buggy and fixed kernels produce identical scheduling decisions.
