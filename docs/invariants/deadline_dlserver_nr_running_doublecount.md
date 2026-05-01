# enqueue/dequeue nr_running delta must be exactly 1
**Source bug:** `52d15521eb75f9b521744db675bee61025d2fa52`

**Property:** A single `enqueue_task()` call for one task must increase `rq->nr_running` by exactly 1, and a single `dequeue_task()` call must decrease it by exactly 1.

**Variables:**
- `nr_before` — `rq->nr_running` immediately before `enqueue_task()` or `dequeue_task()` is called. Recorded at the entry of `enqueue_task()` / `dequeue_task()` in `kernel/sched/core.c`. Read directly from `rq->nr_running`.
- `nr_after` — `rq->nr_running` immediately after `enqueue_task()` or `dequeue_task()` returns. Recorded at the exit of the same functions. Read directly from `rq->nr_running`.

**Check(s):**

Check 1: Performed at the return of `enqueue_task()`. Only when the enqueue is for a real task addition (not a `ENQUEUE_RESTORE` requeue).
```c
// At entry of enqueue_task():
unsigned int nr_before = rq->nr_running;

// At exit of enqueue_task():
unsigned int nr_after = rq->nr_running;
if (!(flags & ENQUEUE_RESTORE))
    WARN_ON_ONCE(nr_after != nr_before + 1);
```

Check 2: Performed at the return of `dequeue_task()`. Only when the dequeue is for a real task removal (not a `DEQUEUE_SAVE` temporary removal).
```c
// At entry of dequeue_task():
unsigned int nr_before = rq->nr_running;

// At exit of dequeue_task():
unsigned int nr_after = rq->nr_running;
if (!(flags & DEQUEUE_SAVE))
    WARN_ON_ONCE(nr_after != nr_before - 1);
```

**Example violation:** When a CFS task is enqueued onto an idle CPU, `enqueue_task_fair()` internally triggers `dl_server_start()` which calls `add_nr_running()` for the proxy dl_server entity, and then `enqueue_task_fair()` itself calls `add_nr_running()` again for the actual task. The net delta is +2 instead of +1.

**Other bugs caught:** Any future bug where internal scheduling helpers (dl_server, bandwidth timers, proxy entities) incorrectly modify `rq->nr_running` as a side effect during a single enqueue/dequeue operation.
