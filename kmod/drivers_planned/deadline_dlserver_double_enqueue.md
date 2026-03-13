# Deadline: dl_server Double Enqueue Due to Missing Active-State Tracking

**Commit:** `b53127db1dbf7f1047cf35c10922d801dcd40324`
**Affected files:** `include/linux/sched.h`, `kernel/sched/deadline.c`, `kernel/sched/sched.h`
**Fixed in:** v6.13-rc3
**Buggy since:** v6.8-rc1 (commit `63ba8422f876` "sched/deadline: Introduce deadline servers")

## Bug Description

The deadline server (dl_server / fair_server) is a `sched_dl_entity` that acts as a proxy scheduling entity for the CFS runqueue. It provides SCHED_DEADLINE bandwidth guarantees to CFS tasks, preventing starvation when higher-priority RT tasks are present. The fair_server is started via `dl_server_start()` when the first CFS task is enqueued on an idle CPU, and stopped via `dl_server_stop()` when the last CFS task departs.

The bug is that the dl_server can be enqueued into the DL runqueue twice (double enqueue), which triggers a `WARN_ON_ONCE(on_dl_rq(dl_se))` in `enqueue_dl_entity()` and corrupts the DL runqueue's red-black tree data structure. This happens because there is no explicit tracking of whether the dl_server is currently active (started) or stopped. Various code paths in `__pick_task_dl()` and `dl_server_start()` make decisions about enqueueing/yielding/updating the dl_server based on stale or inconsistent state in the `dl_throttled` and `dl_yielded` flags, which can lead to a second enqueue of an entity that is already on the runqueue.

Two distinct scenarios can trigger the double enqueue. **Case 1** involves the delayed dequeue feature (introduced in v6.8) interacting with the dl_server pick path: during `__pick_task_dl()`, the dl_server's `server_pick_task()` callback invokes `pick_task_fair()` → `pick_next_entity()`, which may encounter a `sched_delayed` entity and call `dequeue_entities()` → `dl_server_stop()`. This stops and dequeues the dl_server *underneath* the `__pick_task_dl()` function, which then proceeds to set `dl_yielded = 1` and call `update_curr_dl_se()` on the now-dequeued dl_server, corrupting its state. **Case 2** is a race condition between two CPUs where one CPU is in `schedule()` with the rq lock temporarily dropped during `sched_balance_newidle()`, and another CPU performs a `try_to_wake_up()` that enqueues a CFS task and starts the dl_server — followed by `wakeup_preempt()` → `update_curr()` → `dl_server_update()` → `enqueue_dl_entity()` which attempts a second enqueue.

The interaction between Case 1 and Case 2 is particularly pernicious: Case 1 leaves `dl_throttled` and `dl_yielded` set on a stopped dl_server, and then when Case 2's `dl_server_start()` tries to enqueue, the stale `dl_throttled` flag confuses the deferral logic in `enqueue_dl_entity()`, allowing the first enqueue to succeed where it normally would be deferred. The subsequent `dl_server_update()` call then triggers the second enqueue via the replenish path in `update_curr_dl_se()`.

## Root Cause

The root cause is the absence of an authoritative boolean flag indicating whether the dl_server is currently active (i.e., has been started and not yet stopped). Without this flag, the kernel relies on indirect indicators (`dl_throttled`, `dl_yielded`, `on_dl_rq()`) that can become stale or inconsistent in the face of concurrent operations and re-entrant pick paths.

**Case 1 — Delayed dequeue during server pick:**

In `__pick_task_dl()` (line ~2410 in the buggy code), when the dl_server is picked as the next DL entity, it calls `dl_se->server_pick_task(dl_se)`. This eventually calls `pick_task_fair()` → `pick_next_entity()`. If the chosen CFS sched_entity has `sched_delayed == 1`, `pick_next_entity()` calls `dequeue_entities(rq, se, DEQUEUE_SLEEP | DEQUEUE_DELAYED)`. The dequeue may remove the *last* CFS task from the runqueue, which triggers `dl_server_stop(&rq->fair_server)`. Inside `dl_server_stop()`, the dl_server is dequeued from the DL runqueue via `dequeue_dl_entity(dl_se, DEQUEUE_SLEEP)`, `dl_defer_armed` and `dl_throttled` are cleared to 0.

Control then returns to `__pick_task_dl()`. Since `server_pick_task` returned `NULL` (the delayed entity was dequeued, not picked), the code enters the `if (!p)` branch:

```c
if (!p) {
    dl_se->dl_yielded = 1;         // set on already-stopped server!
    update_curr_dl_se(rq, dl_se, 0); // called on already-stopped server!
    goto again;
}
```

Calling `update_curr_dl_se()` with `delta_exec = 0` and `dl_yielded = 1` enters the `throttle:` label, which sets `dl_throttled = 1` and calls `dequeue_dl_entity(dl_se, 0)`. But the dl_server was *already* dequeued by `dl_server_stop()`, so this dequeue operates on a not-on-rq entity. Then, because `!start_dl_timer(dl_se)` may be true and `dl_server(dl_se)` is true, the code calls `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)`, re-enqueueing the stopped server. Now the dl_server is on the DL runqueue with `dl_throttled = 1` and `dl_yielded = 1` — a corrupt state for a server that should be stopped.

**Case 2 — Race condition between CPUs:**

CPU A is in `schedule()` and has called `deactivate_task()` → `dl_server_stop()` (stopping the fair server since the last CFS task blocked), followed by `pick_next_task()` → `pick_next_task_fair()` → `sched_balance_newidle()`, which drops the rq lock via `rq_unlock(this_rq)`.

CPU B wakes a task destined for CPU A's rq via `try_to_wake_up()` → `ttwu_queue()`. It acquires CPU A's rq lock and calls `activate_task()` → `dl_server_start()`. The `dl_server_start()` calls `enqueue_dl_entity(dl_se, ENQUEUE_WAKEUP)` — **first enqueue**. Then `wakeup_preempt()` → `check_preempt_wakeup_fair()` → `update_curr()` → `update_curr_task()` checks `if (current->dl_server)` and calls `dl_server_update()` → `update_curr_dl_se()`. If the dl_server's state was left corrupt by Case 1 (e.g., `dl_throttled` and `dl_yielded` set), `update_curr_dl_se()` may reach `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)` — **second enqueue**, triggering the `WARN_ON_ONCE(on_dl_rq(dl_se))`.

## Consequence

The double enqueue of the dl_server corrupts the deadline runqueue's red-black tree (`dl_rq->root`). The `__enqueue_dl_entity()` function inserts the `sched_dl_entity` node into the rb-tree via `rb_add_cached()`. If the entity is already present in the tree, inserting it again leads to undefined behavior in the rb-tree data structure — at minimum a kernel warning from `WARN_ON_ONCE(on_dl_rq(dl_se))`, and potentially tree corruption that causes infinite loops in rb-tree traversal, list corruption, or other undefined behavior.

The observable symptoms include:
- `WARN_ON_ONCE` splats in `enqueue_dl_entity()` during normal operation, particularly when CFS tasks are frequently waking and sleeping.
- Scheduler time accounting corruption for the dl_server: `dl_throttled` and `dl_yielded` flags become inconsistent, causing incorrect throttling behavior and "DL replenish lagged too much" warnings on subsequent timer firings.
- Potential soft lockups or scheduling anomalies due to the corrupted DL runqueue rb-tree, where `pick_next_dl_entity()` may return stale or incorrect entries.
- The bug was reported and tested-by on a ROCK 5B (ARM64 board), suggesting it manifests on real hardware under normal workloads with sufficient concurrency.

## Fix Summary

The fix introduces an explicit `dl_server_active` bitfield flag in `struct sched_dl_entity` (in `include/linux/sched.h`) and a corresponding `dl_server_active()` inline helper function in `kernel/sched/sched.h`. This flag is set to 1 in `dl_server_start()` immediately before calling `enqueue_dl_entity()`, and cleared to 0 in `dl_server_stop()` after dequeuing. This provides an authoritative, unambiguous indicator of whether the dl_server is currently in the active (started) state.

The critical change is in `__pick_task_dl()`, where the `if (!p)` branch (server_pick_task returned NULL) is now guarded by `if (dl_server_active(dl_se))`:

```c
if (!p) {
    if (dl_server_active(dl_se)) {
        dl_se->dl_yielded = 1;
        update_curr_dl_se(rq, dl_se, 0);
    }
    goto again;
}
```

This means that if the dl_server was stopped during the pick path (Case 1 — delayed dequeue triggered `dl_server_stop()` which clears `dl_server_active`), the yield/update logic is completely skipped. The code simply loops via `goto again`, and since the dl_server is no longer on the DL runqueue (it was properly dequeued by `dl_server_stop()`), `sched_dl_runnable(rq)` will return false (assuming no other DL entities), and `__pick_task_dl()` will return NULL.

Additionally, `dl_server_stop()` now explicitly clears `dl_se->dl_throttled = 0` (was already done before this patch) ensuring that any stale throttled state from a previous interrupted pick is cleaned up. Combined with the `dl_server_active` check, this prevents Case 2 from triggering a double enqueue: even if `dl_server_start()` is called on a dl_server with stale `dl_throttled` state, the flag was cleared by the prior `dl_server_stop()`, and the fresh `dl_server_active = 1` correctly represents the server's new lifecycle.

## Triggering Conditions

To trigger this bug, the following conditions are required:

- **Kernel version**: v6.8-rc1 through v6.13-rc2 (any kernel with commit `63ba8422f876` but without commit `b53127db1dbf7f1047cf35c10922d801dcd40324`). The delayed dequeue feature (also introduced around v6.8) must be present for Case 1.
- **CPU count**: At least 2 CPUs are required for Case 2 (the race condition). Case 1 can theoretically occur on a single CPU.
- **dl_server enabled**: The fair_server must be enabled (default: runtime = 50ms, period = 1000ms). This is the default configuration.
- **Delayed dequeue enabled**: The `DELAY_DEQUEUE` sched feature must be enabled (default in affected kernels).
- **Workload**: At least one CFS task that alternates between sleeping and running. The task must be the *only* CFS task on its CPU so that blocking causes `dl_server_stop()` and waking causes `dl_server_start()`.
- **Case 1 trigger**: A CFS task with `sched_delayed == 1` must be picked by `pick_next_entity()` during a dl_server pick path. This requires a CFS task that was dequeued with the delayed dequeue optimization (i.e., the task blocked but its sched_entity was kept on the runqueue for a deferred cleanup). When the dl_server's `server_pick_task()` encounters this delayed entity, `pick_next_entity()` calls `dequeue_entities()` which removes the last task and triggers `dl_server_stop()`.
- **Case 2 trigger**: The race requires precise timing: CPU A must be in `sched_balance_newidle()` with the rq lock dropped, while CPU B simultaneously calls `try_to_wake_up()` targeting CPU A's rq. This is a natural scenario when a task on CPU A blocks (last CFS task → dl_server stops → newidle balancing), and another CPU wakes a task that migrates to CPU A.
- **Combined trigger**: The most reliable reproduction comes from Case 1 setting corrupt `dl_throttled`/`dl_yielded` state, followed by Case 2 triggering a double enqueue because the stale flags confuse `enqueue_dl_entity()`'s deferral logic.

## Reproduce Strategy (kSTEP)

The key to reproducing this bug is to trigger Case 1: the delayed dequeue path inside the dl_server's pick. This requires a CFS sched_entity with `sched_delayed == 1` to be encountered during `pick_next_entity()` invoked from `pick_task_fair()` → `server_pick_task()` → `__pick_task_dl()`.

### Step-by-step plan:

1. **Topology setup**: Use 2 CPUs (`kstep_topo_init()` with at least 2 CPUs). CPU 0 is reserved for the driver, so CFS tasks will run on CPU 1.

2. **Create CFS tasks**: Create 2 CFS tasks pinned to CPU 1 (`kstep_task_create()` + `kstep_task_pin(p, 1, 1)`). Having two tasks is important: one will become the `sched_delayed` entity, and the other will keep the runqueue non-empty briefly before also departing.

3. **Run tasks to warm up dl_server**: Wake both tasks and tick a few times to ensure the dl_server is active on CPU 1 and has valid timing parameters.

4. **Create the delayed dequeue condition**: The `sched_delayed` flag is set when a task blocks (transitions to TASK_INTERRUPTIBLE/UNINTERRUPTIBLE) but the scheduler defers the actual dequeue. Use `kstep_task_block(task_A)` to block one task. This should leave the sched_entity with `sched_delayed = 1` on the CFS runqueue.

5. **Block the second task**: Now block `task_B` with `kstep_task_block(task_B)`. This leaves only the `sched_delayed` entity of `task_A` on the CFS runqueue.

6. **Force a pick cycle**: At this point, the dl_server is still active (it was started when there were CFS tasks). Use `kstep_tick()` or similar to trigger a scheduler invocation that calls `__pick_next_task()` → `__pick_task_dl()`. The dl_server will be picked from the DL runqueue, and `server_pick_task()` will call `pick_task_fair()` → `pick_next_entity()`. This function encounters the `sched_delayed` entity and calls `dequeue_entities()`, which removes the last task and triggers `dl_server_stop()`. Then control returns to `__pick_task_dl()` with `p == NULL`.

7. **Observe the bug**: In the buggy kernel, `__pick_task_dl()` unconditionally sets `dl_yielded = 1` and calls `update_curr_dl_se(rq, dl_se, 0)` on the now-stopped dl_server. Inside `update_curr_dl_se()`, the `throttle:` label is reached (because `dl_yielded == 1`), which calls `dequeue_dl_entity()` on an already-dequeued entity and then may call `enqueue_dl_entity(dl_se, ENQUEUE_REPLENISH)`, re-enqueueing the stopped server.

8. **Detection via `on_tick_begin` callback or direct state inspection**: Use the `on_tick_begin` or `on_tick_end` callback to inspect the dl_server state after the pick cycle. Check:
   - `rq->fair_server.dl_throttled` — should be 0 after `dl_server_stop()`, but will be 1 in the buggy case.
   - `rq->fair_server.dl_yielded` — should be 0 after `dl_server_stop()`, but will be 1 in the buggy case.
   - Use `KSYM_IMPORT(on_dl_rq)` or directly check `dl_se->rb_node` to see if the dl_server is unexpectedly still on the DL runqueue after being stopped.
   - The `WARN_ON_ONCE(on_dl_rq(dl_se))` in `enqueue_dl_entity()` would fire in the kernel log.

9. **Trigger the double enqueue (Case 2 interaction)**: After Case 1 leaves the dl_server in a corrupt state, wake `task_B` on CPU 1 via `kstep_task_wakeup(task_B)`. This calls `dl_server_start()` → `enqueue_dl_entity(dl_se, ENQUEUE_WAKEUP)`. If the stale `dl_throttled` flag allows the enqueue to proceed (it may not be deferred), and then `wakeup_preempt()` → `update_curr()` → `dl_server_update()` → `update_curr_dl_se()` attempts a second enqueue, the `WARN_ON_ONCE` fires.

10. **Pass/fail criteria**:
    - **FAIL (bug triggered)**: After step 6, inspect `rq->fair_server.dl_throttled` or `rq->fair_server.dl_yielded` — if either is 1 when the server should be stopped, the bug is triggered. Alternatively, check `dmesg` for `WARN_ON` messages from `enqueue_dl_entity()`.
    - **PASS (bug fixed)**: After step 6 on the fixed kernel, `dl_server_active` will be 0 (cleared by `dl_server_stop()`), so the `if (dl_server_active(dl_se))` guard skips the yield/update, and the dl_server remains cleanly stopped with `dl_throttled = 0`, `dl_yielded = 0`.

### kSTEP-specific considerations:

- Access `rq->fair_server` via `cpu_rq(1)->fair_server` using the internal.h access to `struct rq`.
- Use `KSYM_IMPORT` if needed to access `on_dl_rq()` or similar helpers, though direct field inspection of `dl_se->dl_throttled` and `dl_se->dl_yielded` should suffice.
- The `sched_delayed` condition requires the kernel's DELAY_DEQUEUE feature to be active. kSTEP's `kstep_task_block()` should set `TASK_INTERRUPTIBLE` and call `deactivate_task()`, which should trigger the delayed dequeue path on supported kernels.
- The timing between task blocking and the subsequent pick cycle is crucial. Using `kstep_tick()` immediately after blocking both tasks ensures the scheduler runs the pick path while the delayed entity is still present.
- For Case 2 reproduction, kSTEP may need to simulate the rq lock drop during `sched_balance_newidle()`. This might require waking a task from CPU 0 (the driver CPU) targeting CPU 1 while CPU 1 is in the newidle balance path. The `kstep_task_wakeup()` from the driver (running on CPU 0) could naturally trigger `try_to_wake_up()` targeting CPU 1's rq, but the timing of the rq lock drop during newidle balancing is non-deterministic.
- For a more deterministic reproduction, focus on Case 1 alone: demonstrate that the dl_server's `dl_throttled` and `dl_yielded` flags are set to 1 after `dl_server_stop()` on the buggy kernel, and remain 0 on the fixed kernel. This proves the stale-state corruption that is the precondition for the double enqueue.
