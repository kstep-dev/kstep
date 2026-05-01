# External p->on_rq Delayed-Dequeue Misclassification
**Source bug:** `cd9626e9ebc77edec33023fe95dab4b04ffc819d`

No generic invariant applicable. The bug is an API/semantic issue in external subsystems (KVM, perf, freezer, RCU, ftrace) misinterpreting `p->on_rq` after delayed dequeue was introduced — no scheduler-internal state invariant is violated, and the fix only changes how external consumers classify task runnability.
