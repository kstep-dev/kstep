# dl_server Start/Stop Overhead
**Source bug:** `cccb45d7c4295bbfeba616582d0249f2d21e6df5`

No generic invariant applicable. This is a performance design flaw (excessive overhead from aggressive dl_server start/stop on every CFS 0↔1 transition), not a scheduler state correctness violation — no invariant over scheduler data structures was broken.
