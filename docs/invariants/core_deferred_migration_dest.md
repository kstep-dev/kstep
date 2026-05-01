# Migration Destination CPU Must Be Active
**Source bug:** `475ea6c60279e9f2ddf7e4cf2648cd8ae0608361`

No generic invariant applicable. Bug requires CPU hotplug (offline CPUs in affinity mask) which is an environmental condition outside normal scheduler state; the core issue is a one-off design flaw (using sentinel `-1` instead of passing the validated dest_cpu through to the stopper), and `__migrate_task()` already checks `is_cpu_allowed()` — the real problem is silent failure semantics, not a missing state invariant.
