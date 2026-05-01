# dl_server CPU Offline Invariant
**Source bug:** `ee6e44dfe6e50b4a5df853d933a96bdff5309e6e`

No generic invariant applicable. Bug is a CPU hotplug race condition; the violated property (dl_server must not be active on an offline CPU) is only checkable in the hotplug teardown path, not at normal scheduler hook points, and is too specific to hotplug sequencing to catch a broader class of bugs.
