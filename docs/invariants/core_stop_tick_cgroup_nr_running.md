# CFS Tick-Stop Must Respect Hierarchical Task Count
**Source bug:** `c1f43c342e1f2e32f0620bf2e972e2a9ea0a1e60`

No generic invariant applicable. Wrong-variable bug in a single function (`nr_running` vs `h_nr_running`); the property "use hierarchical counts when reasoning about total CFS tasks" is a code correctness rule, not a runtime-checkable state invariant that generalizes beyond this call site.
