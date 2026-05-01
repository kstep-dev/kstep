# Energy uclamp spare cap zero
**Source bug:** `6b00a40147653c8ea748e8f4396510f252763364`

No generic invariant applicable. This is a sentinel-value conflation bug (0 used as both "uninitialized" and valid spare capacity) in local variables of `find_energy_efficient_cpu()`; it is a one-off logic error in a single function with no externally observable scheduler state violation.
