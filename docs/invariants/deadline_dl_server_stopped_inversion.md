# DL Server Stopped Inversion
**Source bug:** `4717432dfd99bbd015b6782adca216c6f9340038`

No generic invariant applicable. Bug is a one-off boolean return-value inversion (`return false` instead of `return true`) in a newly introduced helper function — a typo-level logic error whose observable symptom (`dl_yielded` set on a stopped server) is too specific to the dl_server lazy-shutdown state machine to generalize.
