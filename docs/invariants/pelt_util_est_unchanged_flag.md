# util_est UTIL_AVG_UNCHANGED Flag Leak
**Source bug:** `68d7a190682aa4eb02db477328088ebad15acc83`

No generic invariant applicable. This is a bit-packing implementation bug where an internal flag (UTIL_AVG_UNCHANGED) stored in the LSB of util_est.enqueued leaked through the `_task_util_est()` accessor; it is a one-off coding error in flag masking, not a violation of a reusable scheduler state property.
