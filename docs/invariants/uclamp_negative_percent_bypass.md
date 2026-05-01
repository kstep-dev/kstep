# Uclamp Utilization Value Range Bound
**Source bug:** `b562d140649966d4daedd0483a8fe59ad3bb465a`

No generic invariant applicable. This is an input-validation type mismatch (signed/unsigned comparison) in the cgroup sysfs write path; the violated property is "reject invalid user input," which is not a scheduler-state invariant checkable at runtime hooks.
