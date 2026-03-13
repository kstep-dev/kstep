# RT: Dead BUG_ON in pick_next_rt_entity misses empty queue corruption

**Commit:** `7c4a5b89a0b5a57a64b601775b296abf77a9fe97`
**Affected files:** kernel/sched/rt.c
**Fixed in:** v6.3-rc1
**Buggy since:** v2.6.25-rc1 (commit 326587b84078 "sched: fix goto retry in pick_next_task_rt()")

## Bug Description

The RT scheduler's `pick_next_rt_entity()` function selects the highest-priority runnable RT scheduling entity by looking up the first set bit in `rt_rq->active.bitmap` (an `rt_prio_array`), then extracting the first entry from the corresponding queue. After commit 326587b84078 removed the `sched_rt_ratio_exceeded()` check (and the `goto retry` loop) from `pick_next_rt_entity()`, the function could no longer return NULL through any normal code path. However, the caller `_pick_next_task_rt()` retained a `BUG_ON(!rt_se)` check that was intended to catch NULL returns.

The problem is that `list_entry()` — which is an alias for `container_of()` — is a compile-time pointer arithmetic macro. It computes the address of the containing structure by subtracting the offset of the member field from the given pointer. This computation never produces NULL; even when applied to a bogus pointer, it produces a non-NULL but invalid address. Therefore, the `BUG_ON(!rt_se)` check was dead code that could never trigger, regardless of whether the underlying queue was empty or not.

The real danger was that if the priority bitmap and the queue list somehow became desynchronized — the bitmap indicating tasks exist at a priority level but the corresponding `list_head` queue being empty — then `list_entry(queue->next, struct sched_rt_entity, run_list)` would compute a pointer using `queue->next`, which for an empty list points back to `queue` itself (a `list_head` embedded in the `rt_prio_array`). The resulting bogus pointer would be treated as a valid `sched_rt_entity`, leading to silent memory corruption rather than any crash or diagnostic message.

This commit replaces the dead `BUG_ON` with a proper `SCHED_WARN_ON(list_empty(queue))` check inside `pick_next_rt_entity()` that detects the actual error condition (an empty queue), and converts the caller to gracefully return NULL instead of crashing.

## Root Cause

The root cause is a mismatch between the error being checked and the actual possible failure mode, introduced by commit 326587b84078.

Before 326587b84078, `pick_next_rt_entity()` could return NULL when `sched_rt_ratio_exceeded()` was true, because the function had an early `goto out` that skipped the `list_entry()` call, leaving `next` at its initialized value of NULL. The `goto retry` loop in `pick_next_task_rt()` handled this NULL return by looping back. Commit 326587b84078 removed the `sched_rt_ratio_exceeded()` call from `pick_next_rt_entity()` and moved the throttling check to the top of `pick_next_task_rt()`, reasoning that throttled RT runqueues wouldn't reach the entity selection logic. After this refactoring, `pick_next_rt_entity()` always executed:

```c
idx = sched_find_first_bit(array->bitmap);
BUG_ON(idx >= MAX_RT_PRIO);
queue = array->queue + idx;
next = list_entry(queue->next, struct sched_rt_entity, run_list);
return next;
```

Since `list_entry()` is `container_of()` — a macro that performs `(type *)((char *)(ptr) - offsetof(type, member))` — it always produces a non-NULL pointer as long as its input is non-NULL. For an empty list, `queue->next == queue` (points to itself), and `container_of(queue, struct sched_rt_entity, run_list)` produces a pointer to a memory location that is NOT a real `sched_rt_entity` but rather some offset into the `rt_prio_array.queue[]` array. This is a valid non-NULL pointer to garbage data.

The `BUG_ON(!rt_se)` in the caller `_pick_next_task_rt()` became unreachable dead code:

```c
do {
    rt_se = pick_next_rt_entity(rt_rq);
    BUG_ON(!rt_se);           /* DEAD CODE: list_entry() never returns NULL */
    rt_rq = group_rt_rq(rt_se);
} while (rt_rq);
```

The only real error condition that could occur — the queue being empty despite the bitmap indicating otherwise — was not being detected at all. Such a bitmap/queue desync is an internal invariant violation that should never happen during correct operation, but if it did (due to memory corruption, a separate kernel bug, or a race condition), the system would proceed with a bogus `sched_rt_entity` pointer rather than detecting the problem.

## Consequence

If the RT priority bitmap ever became desynchronized from the queue lists (e.g., a bit set in `rt_rq->active.bitmap` but the corresponding `queue[idx]` being empty), the scheduler would interpret an arbitrary memory location — specifically, a `list_head` within the `rt_prio_array.queue[]` array — as a `struct sched_rt_entity`. Subsequent operations on this bogus entity (accessing `rt_se->parent`, `rt_se->run_list`, the task structure via `rt_task_of(rt_se)`, etc.) would read and write arbitrary kernel memory, potentially causing:

- Silent data corruption of unrelated kernel structures
- A kernel oops or panic when dereferencing invalid pointers from the bogus `sched_rt_entity` fields
- Security vulnerabilities if an attacker could control the contents of the memory being misinterpreted
- Unpredictable scheduling behavior leading to priority inversion, deadlock, or task starvation

Because the `BUG_ON` was dead code, the kernel had no mechanism to detect this invariant violation early. The fix ensures that an empty queue is detected immediately with a `SCHED_WARN_ON` diagnostic, and the scheduler gracefully returns NULL, preventing the corruption cascade. In the worst case after the fix, the RT task simply isn't picked for scheduling in that cycle, which is far better than memory corruption.

## Fix Summary

The fix makes two changes in `kernel/sched/rt.c`:

1. **In `pick_next_rt_entity()`**: After computing the queue pointer from the bitmap index, the fix adds `if (SCHED_WARN_ON(list_empty(queue))) return NULL;` before the `list_entry()` call. This checks the actual error condition — whether the queue is empty — rather than the impossible condition of `list_entry()` returning NULL. `SCHED_WARN_ON()` emits a kernel warning with a backtrace (under `CONFIG_SCHED_DEBUG`), providing diagnostic information without crashing the system.

2. **In `_pick_next_task_rt()`**: The `BUG_ON(!rt_se)` is replaced with `if (unlikely(!rt_se)) return NULL;`. This allows `_pick_next_task_rt()` to propagate the NULL return from `pick_next_rt_entity()` back to its callers (`pick_task_rt()` and `pick_next_task_rt()`), which already handle NULL returns (they check `if (!sched_rt_runnable(rq)) return NULL;` and `if (p) set_next_task_rt(...)`). The `unlikely()` annotation preserves branch prediction optimization since this path should never execute in normal operation.

This fix is correct and complete because it: (a) detects the actual invariant violation (empty queue) rather than an impossible condition (NULL from `list_entry`), (b) degrades gracefully (warn + skip) instead of crashing (`BUG_ON`), and (c) the callers already handle NULL returns, so no further changes are needed up the call chain.

## Triggering Conditions

The bug manifests as dead error-checking code (unreachable `BUG_ON`) combined with the absence of a check for the actual possible error (empty queue despite bitmap indicating otherwise). To trigger the actual dangerous condition that the fix now protects against, the following would be required:

- **Bitmap/queue desynchronization**: The `rt_rq->active.bitmap` would need to have a bit set at index `idx`, but `rt_rq->active.queue[idx]` would need to be an empty list. During normal operation, the bitmap and queues are maintained atomically: `enqueue_rt_entity()` adds to the queue and sets the bitmap bit; `dequeue_rt_entity()` removes from the queue and clears the bit if the queue becomes empty. There is no known sequence of normal RT scheduling operations that can desynchronize these data structures.

- **Potential desync causes**: Such a desync could theoretically be caused by: (a) memory corruption from a hardware fault or a separate kernel bug (e.g., use-after-free, buffer overflow into the `rt_prio_array`), (b) a race condition in a code path not protected by the runqueue lock (all normal bitmap/queue operations hold `rq->lock`, but a bug could violate this), or (c) a bug in RT group scheduling (`CONFIG_RT_GROUP_SCHED`) where group-level throttling/unthrottling operations fail to maintain the invariant.

- **Configuration**: Any kernel configuration with `CONFIG_RT_GROUP_SCHED` or standard RT scheduling is affected. The bug exists in both UP and SMP configurations. No specific CPU count, topology, or NUMA setup is required — the issue is purely about the data structure invariant within a single runqueue.

- **Probability**: Under normal operation, the probability of triggering this is essentially zero. This is a defensive coding improvement, not a fix for a known reproducible issue. The lore thread confirms this — a reviewer asked whether the patch was motivated by a real-world scenario or was a static analysis finding, and no specific reproduction was reported.

## Reproduce Strategy (kSTEP)

This bug **cannot be reproduced** with kSTEP for the following reasons:

### 1. Why it cannot be reproduced

The "bug" is fundamentally a code quality issue: dead error-checking code (`BUG_ON(!rt_se)`) that checks an impossible condition (NULL return from `list_entry()`) while missing the actual possible error condition (empty queue with inconsistent bitmap). There is **no known sequence of normal scheduling API calls** — creating RT tasks, enqueueing, dequeueing, migrating, changing priorities, throttling, etc. — that can cause the `rt_prio_array` bitmap and queues to become desynchronized.

To actually trigger the dangerous path (where `pick_next_rt_entity()` processes an empty queue), you would need to directly corrupt the internal `rt_prio_array` data structure: set a bit in the bitmap without adding an entry to the corresponding queue, or remove an entry from the queue without clearing the bitmap bit. This requires **direct manipulation of internal scheduler state**, which the kSTEP instructions explicitly discourage ("Try not to directly manipulate internal scheduler state").

Furthermore, even if you did corrupt the data structures via `KSYM_IMPORT` and direct memory access, you would be testing the kernel's response to arbitrary memory corruption, not reproducing a real scheduler bug. The patch author did not report any way to trigger this through normal operation, and the reviewers treated it as a static-analysis-motivated hardening patch.

### 2. What would need to be added to kSTEP

To force-trigger this, kSTEP would need the ability to directly corrupt specific fields of `rt_rq->active`:
- Set a bit in `rt_rq->active.bitmap[word]` at a specific priority index
- While ensuring `rt_rq->active.queue[idx]` is empty (or clear the queue after setting the bit)
- Then force a pick_next_task cycle on that CPU

While kSTEP technically provides access to internal structures via `internal.h` and `KSYM_IMPORT`, using these to corrupt bitmap/queue invariants is not a meaningful reproduction — it tests error handling of arbitrary corruption, not a scheduler logic bug.

### 3. Nature of the fix

This is a **defensive coding / code hardening** patch. The key improvements are:
- Replacing dead code (`BUG_ON(!rt_se)` that can never fire) with a meaningful check (`SCHED_WARN_ON(list_empty(queue))`)
- Changing crash behavior (`BUG_ON`) to graceful degradation (`WARN_ON` + return NULL)
- Properly guarding against the only possible error condition in the function

The pre-fix and post-fix kernels behave **identically** under all normal scheduling operations. The difference only manifests if the internal data structures are corrupted, which is not a scenario that kSTEP is designed to test.

### 4. Alternative reproduction methods

- **Fault injection**: Use kernel fault injection (`failslab`, memory corruption injection, or `KFENCE`) to corrupt the `rt_prio_array` during RT task scheduling and verify that the new `SCHED_WARN_ON` fires instead of silently proceeding with a bogus pointer.
- **Static analysis**: The bug can be confirmed via static analysis tools (Coccinelle, Smatch, sparse) that detect dead `BUG_ON` conditions where the checked expression can never be false.
- **Code review**: Manual inspection confirms `container_of()` / `list_entry()` always returns non-NULL, making the original `BUG_ON(!rt_se)` unreachable.
