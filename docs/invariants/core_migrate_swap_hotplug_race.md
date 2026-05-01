# Stop-class Wakeup Deferral Race
**Source bug:** `009836b4fa52f92cba33618e773b1094affa8cd2`

No generic invariant applicable. This is a race condition between wakeup deferral mechanisms (wake_q batching and ttwu_queue_wakelist IPI deferral) and CPU hotplug's balance_push() re-entrancy — the violated property (stopper thread wakeups must be immediate) is specific to the stop_machine/hotplug interaction and not expressible as a checkable predicate over scheduler runqueue state.
