# Core: Forceidle Balance Callback Queued from Wrong Context

**Commit:** `5b6547ed97f4f5dfc23f8e3970af6d11d7b7ed7e`
**Affected files:** kernel/sched/core.c, kernel/sched/idle.c, kernel/sched/sched.h
**Fixed in:** v5.18-rc2
**Buggy since:** v5.14-rc1 (introduced by `d2dfa17bc7de` "sched: Trivial forced-newidle balancer")

## Bug Description

Core scheduling is a Linux kernel feature that ensures only mutually trusted tasks (sharing the same "core cookie") run concurrently on SMT (Simultaneous Multi-Threading) siblings of the same physical core. When no compatible task is available for a sibling, the scheduler forces that sibling idle — this is called "forced idle." To mitigate the throughput loss from forced idle, a "forceidle balancer" (`sched_core_balance()`) was introduced in commit `d2dfa17bc7de` to search for cookie-matching tasks that could fill the idle sibling.

The bug is that `queue_core_balance()` — which queues the forceidle balance callback — was called unconditionally from `set_next_task_idle()` every time the idle task was selected as the next task to run. The `set_next_task()` function (and by extension, the per-class `.set_next_task` callback) is invoked not only from `pick_next_task()` within `__schedule()`, but also from the "change" pattern used by `rt_mutex_setprio()`, `__do_set_cpus_allowed()`, and other functions that modify properties of the currently running task. This means the forceidle balance callback could be queued from contexts outside the main scheduler path.

Steven Rostedt reported that ChromeOS encountered crashes when the forceidle balancer was invoked from `rt_mutex_setprio()`'s `balance_callback()` call. The `rt_mutex_setprio()` function uses the dequeue/put_prev → change → enqueue/set_next "change" pattern to modify a task's priority (for PI boosting). At the end of `rt_mutex_setprio()`, it calls `balance_callback()` to process any queued balance callbacks. If the forceidle balance callback had been queued (from a prior `set_next_task_idle()` invocation that went through this code path), `sched_core_balance()` would execute under the `rt_mutex_setprio()` rq->lock instance, not the `__schedule()` rq->lock instance. This violated internal locking invariants and caused a crash.

The fix moves `queue_core_balance()` out of `set_next_task_idle()` and into `pick_next_task()`, gating it with the precise condition `rq->core->core_forceidle_count && next == rq->idle`. This ensures the callback is only ever queued from within the `__schedule()` context, where the rq->lock instance is correct for executing balance callbacks, and only when the CPU is genuinely being forced idle.

## Root Cause

The root cause is that `queue_core_balance()` was placed in `set_next_task_idle()` — the idle scheduling class's `.set_next_task` callback — without considering that this callback is invoked from multiple contexts beyond `__schedule()`.

In the buggy code, `set_next_task_idle()` in `kernel/sched/idle.c` was:

```c
static void set_next_task_idle(struct rq *rq, struct task_struct *next, bool first)
{
    update_idle_core(rq);
    schedstat_inc(rq->sched_goidle);
    queue_core_balance(rq);   /* BUG: unconditional call */
}
```

The `set_next_task()` function is used in two fundamentally different contexts:

1. **From `pick_next_task()` within `__schedule()`**: This is the normal scheduler path. After task selection, `set_next_task(rq, next)` is called to install the chosen task. If the chosen task is the idle task, `set_next_task_idle()` fires. In this context, `balance_callback()` is called at the end of `__schedule()` (via `finish_task_switch()` or the same-task optimization path), and the callbacks run under the correct `__schedule()` rq->lock instance. This is the correct context for `sched_core_balance()`.

2. **From the "change" pattern**: Several functions (most notably `rt_mutex_setprio()` and `__do_set_cpus_allowed()`) use a pattern to modify properties of a currently running task:

```c
queued = task_on_rq_queued(p);
running = task_current(rq, p);
if (queued) dequeue_task(...);
if (running) put_prev_task(...);
/* change task properties */
if (queued) enqueue_task(...);
if (running) set_next_task(...);
```

While `rt_mutex_setprio()` will never operate on the idle task itself (priority boosting the idle task is nonsensical), the `set_next_task()` call in the change pattern could theoretically trigger the idle class callback in edge cases. More importantly, `rt_mutex_setprio()` calls `balance_callback()` at its end:

```c
void rt_mutex_setprio(struct task_struct *p, struct task_struct *pi_task)
{
    ...
    __task_rq_unlock(rq, &rf);
    balance_callback(rq);   /* processes any queued callbacks! */
    preempt_enable();
}
```

This `balance_callback()` invocation processes ALL queued balance callbacks on the rq, including any `core_balance_head` callback that may have been queued previously. If the forceidle balance callback was queued during a prior `set_next_task_idle()` call and was still pending when `rt_mutex_setprio()` runs its `balance_callback()`, the `sched_core_balance()` function executes under the wrong rq->lock context.

The `sched_core_balance()` function performs complex operations including releasing and re-acquiring the rq lock (`raw_spin_rq_unlock_irq(rq)` / `raw_spin_rq_lock_irq(rq)`), traversing sched domains, and stealing tasks across CPUs. These operations carry assumptions about the lock state and caller context that are violated when running from `rt_mutex_setprio()`.

Additionally, the original code was strictly too aggressive: it queued the forceidle balancer on every idle task selection, even when the CPU was not actually being forced idle (e.g., when the CPU was simply idle with no runnable tasks). The `queue_core_balance()` function has internal guards (`!sched_core_enabled(rq)`, `!rq->core->core_cookie`, `!rq->nr_running`), but calling it unconditionally was wasteful and fragile.

## Consequence

The observable consequence is a kernel crash on systems running core scheduling with SMT. Steven Rostedt reported this on ChromeOS systems. The crash occurs because `sched_core_balance()` runs from an unexpected context (`rt_mutex_setprio()`'s `balance_callback()`) where the rq->lock state invariants are violated.

Specifically, the `sched_core_balance()` function drops the rq lock (`raw_spin_rq_unlock_irq(rq)`), performs cross-CPU task stealing via `try_steal_cookie()` (which acquires double rq locks), and then re-acquires the original rq lock. This sequence is designed to run within `__schedule()`'s rq->lock context. When it runs from `rt_mutex_setprio()`'s `balance_callback()`, the lock nesting and ownership assumptions are different. The `rq_pin_lock()` mechanism, which guards against exactly this kind of out-of-band balance callback execution, should trigger a WARN_ON. On production ChromeOS systems, this manifested as a crash ("explodes" in Peter Zijlstra's words).

Beyond the crash, the bug also had a performance implication: the forceidle balancer was queued on every transition to idle, not just when the CPU was genuinely forced idle. This meant the potentially expensive `sched_core_balance()` function (which traverses sched domains and attempts cross-CPU task stealing) could run unnecessarily, wasting cycles.

## Fix Summary

The fix moves the `queue_core_balance()` call from `set_next_task_idle()` in `kernel/sched/idle.c` to `pick_next_task()` in `kernel/sched/core.c`, adding a precise condition guard.

In `kernel/sched/idle.c`, the `queue_core_balance()` call is simply removed from `set_next_task_idle()`:

```c
static void set_next_task_idle(struct rq *rq, struct task_struct *next, bool first)
{
    update_idle_core(rq);
    schedstat_inc(rq->sched_goidle);
    /* queue_core_balance(rq) removed */
}
```

In `kernel/sched/core.c`, the core scheduling `pick_next_task()` function is restructured. The old `done:` label (which led to `set_next_task()` followed by `return next`) is split into two labels: `out_set_next:` (calls `set_next_task()` then falls through) and `out:` (common exit point). The `queue_core_balance()` call is placed at `out:` with a precise condition:

```c
out_set_next:
    set_next_task(rq, next);
out:
    if (rq->core->core_forceidle_count && next == rq->idle)
        queue_core_balance(rq);
    return next;
```

The early-return path (where `rq->core_pick` is still valid from a previous pick) is changed from `return next` to `goto out`, ensuring that even cached picks properly trigger the forceidle balancer when appropriate.

Additionally, `queue_core_balance()` is changed from a globally visible function (`void queue_core_balance()` with `extern` declaration in `sched.h`) to a `static` function in `core.c`, and the empty stub for the `!CONFIG_SCHED_CORE` case is removed from `sched.h`. This is correct because the function is now only called from within `pick_next_task()` in `core.c`.

This fix is correct because `pick_next_task()` is only called from `__schedule()`, guaranteeing the correct rq->lock instance. The condition `rq->core->core_forceidle_count && next == rq->idle` precisely identifies when a CPU is being forced idle (there are other CPUs in the core that are forced idle, and this CPU is about to run the idle task), which is the only situation where the forceidle balancer needs to run.

## Triggering Conditions

The following conditions are required to trigger this bug:

- **CONFIG_SCHED_CORE enabled**: The entire core scheduling subsystem, including `queue_core_balance()` in `set_next_task_idle()`, is compiled only when `CONFIG_SCHED_CORE=y`.
- **SMT topology**: Core scheduling operates on SMT siblings. At least 2 SMT threads per physical core are required. The system must have SMT enabled.
- **Core scheduling active**: Core scheduling must be enabled at runtime. This is done via `prctl(PR_SCHED_CORE, ...)` to assign core cookies to tasks. The `sched_core_get()` function must have been called (happens automatically when the first task gets a core cookie).
- **Core cookies assigned**: Tasks must have non-zero `core_cookie` values assigned, creating trust domains. At least two tasks with different (or zero vs non-zero) cookies must be runnable on SMT siblings of the same physical core, so that one sibling is forced idle.
- **Forced idle state**: A CPU must be forced idle — selected to run the idle task despite having runnable tasks — because no cookie-compatible task is available for it while its SMT sibling runs a cookied task. This triggers `set_next_task_idle()` → `queue_core_balance()`.
- **rt_mutex priority inheritance**: An `rt_mutex_setprio()` call must occur on the same CPU where the forceidle balance callback is queued. This happens when a task holding an `rt_mutex` gets priority-boosted because a higher-priority task blocks on the mutex. The `rt_mutex_setprio()` function calls `balance_callback()` at its end, which processes the stale `core_balance_head` callback.
- **Timing**: The forceidle balance callback must be queued (via `set_next_task_idle()`) and then the `rt_mutex_setprio()` must run on the same CPU before the callback is consumed by a normal `__schedule()` `balance_callback()` invocation. This is a race condition, but on ChromeOS systems with core scheduling and rt_mutex usage (e.g., via futex PI), it was reliably triggered.

The bug is most likely to occur on systems that combine core scheduling (for security) with real-time workloads or applications that use PI futexes (which internally use `rt_mutex`), such as ChromeOS. The probability of hitting the crash depends on the frequency of forced idle events coinciding with PI priority inheritance operations.

## Reproduce Strategy (kSTEP)

This bug can be reproduced in kSTEP by setting up an SMT topology with core scheduling enabled, creating a forced-idle scenario via cookie mismatches, and then triggering `rt_mutex_setprio()` via kernel rt_mutex contention to fire the wrongly-queued balance callback.

### Prerequisites

1. **Enable CONFIG_SCHED_CORE**: The kSTEP default config has `# CONFIG_SCHED_CORE is not set`. A custom config fragment must be provided via `KSTEP_EXTRA_CONFIG` containing `CONFIG_SCHED_CORE=y`. This ensures the core scheduling code paths are compiled in.

2. **QEMU SMT topology**: Configure QEMU with at least 4 CPUs organized as 2 physical cores × 2 SMT threads each. CPU 0 is reserved for the driver, so the test uses CPUs 1-3 (core 0: CPUs 0,1; core 1: CPUs 2,3). Use `kstep_topo_init()` and `kstep_topo_set_smt()` to set up the topology.

### Step-by-step Plan

1. **Set up SMT topology**:
   - Call `kstep_topo_init()` to initialize topology.
   - Use `kstep_topo_set_smt(0, 1)` to make CPUs 0 and 1 SMT siblings.
   - Use `kstep_topo_set_smt(2, 3)` to make CPUs 2 and 3 SMT siblings.
   - Call `kstep_topo_apply()`.

2. **Enable core scheduling**:
   - Use `KSYM_IMPORT(sched_core_get)` and call `sched_core_get()` to enable core scheduling globally.
   - Alternatively, use the internal `sched.h` header (available via `internal.h`) to access `sched_core_enabled()` and related functions.

3. **Create tasks with cookie mismatches**:
   - Create kthread A (cookied): `kstep_kthread_create("task_a")`. Bind to CPU 2 via `kstep_kthread_bind()`. After creation, directly set `task_a->core_cookie = 0x1234` via the internal task_struct access. Use `KSYM_IMPORT(sched_core_enqueue)` to add it to the core tree.
   - Create kthread B (different cookie or uncookied): `kstep_kthread_create("task_b")`. Bind to CPU 3 (SMT sibling of CPU 2). Set `task_b->core_cookie = 0` (or a different cookie). Start it.
   - When task A runs on CPU 2 with cookie 0x1234 and task B on CPU 3 has no matching cookie, CPU 3 is forced idle. This triggers `set_next_task_idle()` → `queue_core_balance()` on CPU 3.

4. **Set up rt_mutex contention to trigger rt_mutex_setprio()**:
   - Declare a `DEFINE_RT_MUTEX(test_mutex)` in the driver.
   - Create kthread C (low priority, SCHED_FIFO): `kstep_kthread_create("locker")`. Bind to CPU 3. Have it acquire `rt_mutex_lock(&test_mutex)` and then busy-wait or sleep.
   - Create kthread D (high priority, SCHED_FIFO): `kstep_kthread_create("blocker")`. Bind to CPU 3. Have it attempt `rt_mutex_lock(&test_mutex)`, which will trigger PI boost on kthread C via `rt_mutex_setprio()`.
   - When `rt_mutex_setprio()` runs for kthread C on CPU 3, it calls `balance_callback()` at its end. If the forceidle `core_balance_head` was previously queued on CPU 3 by `set_next_task_idle()`, it will be picked up and `sched_core_balance()` will execute from the wrong context.

5. **Detection**:
   - On the **buggy kernel**: The `sched_core_balance()` callback fires from `rt_mutex_setprio()`'s `balance_callback()`. This should trigger the `WARN` in `rq_pin_lock()` (which detects out-of-band balance callbacks) or cause a crash. Check `dmesg` / kernel log for warnings, BUG, or oops. Use `kstep_fail()` if a warning or crash trace is detected.
   - On the **fixed kernel**: The `queue_core_balance()` is never called from `set_next_task_idle()`, so no stale callback is queued. The forceidle balance callback is only queued from `pick_next_task()` within `__schedule()`, and is properly consumed. No warning or crash occurs. Use `kstep_pass()`.

6. **Alternative simpler detection** (if the crash is hard to trigger deterministically):
   - Add a `printk` probe or use `KSYM_IMPORT` to read the `core_balance_head` callback state on the rq after `set_next_task_idle()` runs.
   - On the buggy kernel, observe that `core_balance_head` is queued (non-NULL `.next` or present in `rq->balance_callback` list) after idle selection even when `core_forceidle_count == 0`.
   - On the fixed kernel, observe that `core_balance_head` is only queued when `core_forceidle_count > 0` and the idle task is selected.
   - Use the `on_tick_begin` or `on_sched_softirq_end` callback to inspect `rq->balance_callback` state and check whether `core_balance_head` is inappropriately queued.

### Key Implementation Details

- **rt_mutex from kernel module**: The `rt_mutex` API (`rt_mutex_init()`, `rt_mutex_lock()`, `rt_mutex_unlock()`) is available in kernel modules. Define a static `DEFINE_RT_MUTEX(test_mutex)` and have kthreads contend on it.
- **Core cookie manipulation**: Since kSTEP's internal.h includes `kernel/sched/sched.h`, the driver has direct access to `task_struct->core_cookie`, `rq->core_tree`, and related structures. Use `KSYM_IMPORT(sched_core_enqueue)` and `KSYM_IMPORT(sched_core_dequeue)` to manage the core tree.
- **KSTEP_EXTRA_CONFIG**: Create a config fragment file with `CONFIG_SCHED_CORE=y` and pass it via `KSTEP_EXTRA_CONFIG` when building the kernel.
- **Guard with LINUX_VERSION_CODE**: Use `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)` to guard the driver, since the bug exists from v5.14-rc1 (when core scheduling and the forceidle balancer were introduced) through v5.18-rc1.

### Expected Behavior

- **Buggy kernel (pre-v5.18-rc2)**: The forceidle balance callback is queued every time the idle task is selected via `set_next_task_idle()`. When `rt_mutex_setprio()` subsequently calls `balance_callback()`, it finds and executes the stale `core_balance_head` callback, running `sched_core_balance()` from the wrong context. This triggers a WARN from `rq_pin_lock()` or a crash. The driver should detect this via dmesg or by observing the inappropriate callback queuing.
- **Fixed kernel (v5.18-rc2+)**: The `queue_core_balance()` call is only made from `pick_next_task()` with the condition `core_forceidle_count && next == rq->idle`. The callback is always consumed within the same `__schedule()` invocation. No stale callbacks exist for `rt_mutex_setprio()` to pick up. No WARN or crash occurs.
