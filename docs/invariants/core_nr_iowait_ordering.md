# Non-negative runqueue iowait counter
**Source bug:** `ec618b84f6e15281cc3660664d34cd0dd2f2579e`

**Property:** `rq->nr_iowait` must always be non-negative, since it counts the number of tasks in I/O wait state on that runqueue.

**Variables:**
- `nr_iowait` — current value of `rq->nr_iowait` (atomic_t). Read in-place via `atomic_read(&rq->nr_iowait)` at each check point. No shadow variable needed.

**Check(s):**

Check 1: Performed at `scheduler_tick` (called once per tick per CPU). No preconditions.
```c
int iowait = atomic_read(&rq->nr_iowait);
if (iowait < 0)
    /* VIOLATION: nr_iowait underflowed — decrement raced ahead of increment */
```

Check 2: Performed at `ttwu_do_activate`, after the `nr_iowait` decrement (if taken). No preconditions.
```c
int iowait = atomic_read(&task_rq(p)->nr_iowait);
if (iowait < 0)
    /* VIOLATION: nr_iowait went negative after ttwu decrement */
```

**Example violation:** The buggy `try_to_wake_up()` decrements `rq->nr_iowait` before `__schedule()` on the sleeping CPU has incremented it, causing the counter to transiently (or persistently under load) become negative, producing wildly incorrect IO-wait statistics and inflated load averages.

**Other bugs caught:** Potentially `core_nr_uninterruptible_overflow` (similar non-negative counter pattern for `rq->nr_uninterruptible`), and any future bug causing double-decrement or misordered decrement of `nr_iowait`.
