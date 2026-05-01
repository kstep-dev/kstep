# Cpufreq EAS Rebuild SD Skip
**Source bug:** `70d8b6485b0bcd135b6699fc4252d2272818d1fb`

No generic invariant applicable. This is a control-flow bug (misplaced goto label skipping a function call) in cpufreq governor initialization, not a violation of scheduler data structure consistency.
