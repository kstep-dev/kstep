# NUMA numab_state Leak
**Source bug:** `5f1b64e9a9b7ee9cfd32c6b2fab796e29bfed075`

No generic invariant applicable. This is a TOCTOU race condition on lazy pointer initialization (concurrent threads overwrite `vma->numab_state` without freeing the previous allocation); the violated property is memory safety under concurrency, not a scheduler state invariant checkable via scheduler hooks.
