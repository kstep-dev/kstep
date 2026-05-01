# NUMA Imbalance Wrong Argument
**Source bug:** `233e7aca4c8a2c764f556bba9644c36154017e7f`

No generic invariant applicable. This is a wrong-variable bug (passing `src_running` instead of `dst_running` to `adjust_numa_imbalance()`); the error is a one-off argument mixup with no checkable state invariant — the correct vs incorrect argument are both valid scheduler values, and the inconsistency between NUMA balancer and load balancer decisions is not expressible as a predicate over runtime scheduler state.
