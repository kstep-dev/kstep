# cpus_share_cache Race
**Source bug:** `42dc938a590c96eeb429e1830123fef2366d9c80`

No generic invariant applicable. Race condition in a helper function (`cpus_share_cache`) caused by unsynchronized reads of `sd_llc_id` during concurrent domain updates; the fix is a simple identity short-circuit (`this_cpu == that_cpu → true`) with no generalizable scheduler state property to check.
