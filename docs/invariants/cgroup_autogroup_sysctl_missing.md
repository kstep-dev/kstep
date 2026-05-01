# Autogroup Sysctl Missing
**Source bug:** `82f586f923e3ac6062bc7867717a7f8afc09e0ff`

No generic invariant applicable. This is a boot-time init-ordering mistake (sysctl registration placed in wrong function); no scheduler runtime state property is violated.
