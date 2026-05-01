# EAS Early Exit Must Not Skip Tasks With Nonzero Actual Utilization
**Source bug:** `23c9519def98ee0fa97ea5871535e9b136f522fc`

No generic invariant applicable. This is a logic error in a single early-exit condition in `find_energy_efficient_cpu()` where uclamp clamping caused a nonzero-utilization task to appear as zero-utilization; the "invariant" would just restate the correct condition for one specific code path and is not generalizable beyond feec's early exit.
