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

This bug can be reproduced in kSTEP by setting up an SMT topology with core scheduling enabled, creating a forced-idle scenario via cookie mismatches using the proper kernel `sched_core_share_pid()` API, and then triggering `rt_mutex_setprio()` via kernel rt_mutex contention to fire the wrongly-queued balance callback. Crucially, all core-cookie assignment is done through the kernel's own core-scheduling API — imported via `KSYM_IMPORT` — rather than by directly writing to `task_struct->core_cookie` or manually calling internal tree-management helpers like `sched_core_enqueue()`. Internal scheduler structures (rq, core_forceidle_count, balance_callback lists) are only ever *read* for observation and verification.

### Prerequisites

1. **Enable CONFIG_SCHED_CORE**: The kSTEP default config has `# CONFIG_SCHED_CORE is not set`. A custom config fragment must be provided via `KSTEP_EXTRA_CONFIG` containing `CONFIG_SCHED_CORE=y`. This ensures the core scheduling code paths — including `queue_core_balance()`, `sched_core_balance()`, the core-cookie RB tree, and the forceidle accounting — are compiled in.

2. **QEMU SMT topology**: Configure QEMU with at least 4 CPUs organized as 2 physical cores × 2 SMT threads each. CPU 0 is reserved for the driver, so the test uses CPUs 1–3 (core 0: CPUs 0,1; core 1: CPUs 2,3). Use `kstep_topo_init()` and `kstep_topo_set_smt()` to set up the topology. The SMT topology is essential because core scheduling operates on SMT siblings, and the `sched_smt_present` static key must be enabled for `sched_core_share_pid()` to accept cookie operations (it returns `-ENODEV` otherwise).

3. **Import the kernel core-scheduling API**: Use `KSYM_IMPORT(sched_core_share_pid)` to import the kernel function that backs the `prctl(PR_SCHED_CORE, ...)` userspace interface. Its signature is `int sched_core_share_pid(unsigned int cmd, pid_t pid, enum pid_type type, unsigned long uaddr)`. When called with `PR_SCHED_CORE_CREATE`, this function internally allocates a new unique cookie via `sched_core_alloc_cookie()`, then sets it on the target task via `sched_core_update_cookie()`, which properly handles rq locking, core-tree enqueue/dequeue, `sched_core_get()` refcounting, and rescheduling. This single API call replaces all manual `core_cookie` field writes, `sched_core_enqueue()` calls, and `sched_core_get()` calls from the previous strategy.

### Step-by-step Plan

1. **Set up SMT topology**:
   - Call `kstep_topo_init()` to initialize the topology builder.
   - Use `kstep_topo_set_smt(0, 1)` to make CPUs 0 and 1 SMT siblings (physical core 0).
   - Use `kstep_topo_set_smt(2, 3)` to make CPUs 2 and 3 SMT siblings (physical core 1).
   - Call `kstep_topo_apply()` to commit the topology. This ensures the `sched_smt_present` static key is set, which is a prerequisite for core scheduling to function. CPU 0 remains reserved for the kSTEP driver; the bug reproduction uses CPUs 2 and 3 (core 1).

2. **Create tasks with cookie mismatches (via kernel API)**:
   - Create kthread A: `task_a = kstep_kthread_create("task_a")`. Bind it to CPU 2 via `kstep_kthread_bind(task_a, cpumask_of(2))`. Start it with `kstep_kthread_start(task_a)`.
   - **Assign a core cookie to task A through the proper API**: Call `KSYM_IMPORT(sched_core_share_pid)` with arguments `(PR_SCHED_CORE_CREATE, task_pid_nr(task_a), PIDTYPE_PID, 0)`. This allocates a brand-new cookie and assigns it to task A through the full kernel code path: `sched_core_alloc_cookie()` → `__sched_core_set()` → `sched_core_update_cookie()`. The kernel handles rq locking, inserting task A into the `core_tree` RB tree, incrementing the global `sched_core_get()` refcount (which flips the `__sched_core_enabled` static key on), and triggering a reschedule if needed. No direct write to `task_a->core_cookie` is performed by the driver.
   - Create kthread B: `task_b = kstep_kthread_create("task_b")`. Bind it to CPU 3 (SMT sibling of CPU 2) via `kstep_kthread_bind(task_b, cpumask_of(3))`. Start it with `kstep_kthread_start(task_b)`.
   - **Assign a *different* core cookie to task B**: Call `sched_core_share_pid(PR_SCHED_CORE_CREATE, task_pid_nr(task_b), PIDTYPE_PID, 0)` again. Each `PR_SCHED_CORE_CREATE` invocation allocates a distinct cookie, so task B will have a different cookie from task A. This creates the cookie mismatch needed for forced idle.
   - **Verification (read-only)**: After both calls, read `task_a->core_cookie` and `task_b->core_cookie` to confirm they are both non-zero and different. Also read `sched_core_enabled(cpu_rq(2))` to confirm core scheduling is active. These are purely observational reads.

3. **Trigger the forced-idle condition**:
   - With task A (cookie X) runnable on CPU 2 and task B (cookie Y ≠ X) runnable on CPU 3, the core-wide `pick_next_task()` on core 1 will select task A (or B) as `max`, set `core_cookie` to its cookie, then attempt to find a matching task for the sibling. Since no task on CPU 3 matches task A's cookie (and vice versa), the sibling CPU is forced idle — the scheduler selects the idle task for it and increments `rq->core->core_forceidle_count`.
   - On the **buggy kernel**, this idle-task selection goes through `set_next_task_idle()`, which unconditionally calls `queue_core_balance(rq)`, queuing the `core_balance_head` callback onto the rq's `balance_callback` list. This callback is now pending, waiting to be drained by a `balance_callback()` invocation.
   - Advance the scheduler with `kstep_tick()` or `kstep_sleep()` to allow the core-wide pick to execute and establish the forced-idle state. Use `on_tick_end` or poll `cpu_rq(3)->core->core_forceidle_count` (read-only) to confirm that forced idle has been established on the target CPU.

4. **Set up rt_mutex contention to trigger rt_mutex_setprio()**:
   - Declare a `DEFINE_RT_MUTEX(test_mutex)` in the driver module. The `rt_mutex` API (`rt_mutex_init()`, `rt_mutex_lock()`, `rt_mutex_unlock()`) is available to kernel modules without any special imports.
   - Create kthread C (the lock holder, low-priority SCHED_FIFO): `task_c = kstep_kthread_create("locker")`. Bind to CPU 3 via `kstep_kthread_bind(task_c, cpumask_of(3))`. Convert to SCHED_FIFO at a low RT priority using `kstep_task_fifo(task_c)` followed by `kstep_task_set_prio(task_c, 50)`. The kthread function should acquire `rt_mutex_lock(&test_mutex)` and then block or busy-wait, holding the mutex.
   - Create kthread D (the contender, high-priority SCHED_FIFO): `task_d = kstep_kthread_create("blocker")`. Bind to CPU 3. Set it to SCHED_FIFO at a higher RT priority: `kstep_task_fifo(task_d)` then `kstep_task_set_prio(task_d, 10)`. The kthread function should attempt `rt_mutex_lock(&test_mutex)`, which blocks because task C holds the mutex. This triggers priority inheritance: the kernel calls `rt_mutex_setprio()` on task C to boost its priority to match task D's.
   - The sequencing matters: start task C first and ensure it acquires the mutex (use `kstep_tick()` / `kstep_sleep()` to give it time to run and grab the lock). Then start task D, which will block on the mutex and trigger `rt_mutex_setprio()`.

5. **The bug manifests**:
   - When `rt_mutex_setprio()` runs for task C on CPU 3, it uses the "change" pattern: dequeue → put_prev → modify priority → enqueue → set_next. At the end of `rt_mutex_setprio()`, it calls `__task_rq_unlock(rq, &rf)` followed by `balance_callback(rq)` to drain any pending balance callbacks on the rq.
   - On the **buggy kernel**, the `core_balance_head` callback was previously queued by `set_next_task_idle()` during the forced-idle selection in step 3. This stale callback is still sitting on CPU 3's `rq->balance_callback` list. When `balance_callback()` runs from `rt_mutex_setprio()`, it finds and executes `sched_core_balance()` — but now under the *wrong* rq->lock instance (the `rt_mutex_setprio()` context rather than the `__schedule()` context).
   - The `sched_core_balance()` function drops the rq lock (`raw_spin_rq_unlock_irq`), traverses sched domains, attempts cross-CPU task stealing via `try_steal_cookie()` (which acquires double rq locks), then re-acquires the original rq lock. These operations assume the `__schedule()` rq->lock instance. Running them from `rt_mutex_setprio()`'s context violates these invariants, triggering the `WARN_ON` in `rq_pin_lock()` or causing a crash.

6. **Detection**:
   - **Primary detection (crash/WARN)**: On the buggy kernel, the out-of-band `sched_core_balance()` invocation should trigger the `WARN` in `rq_pin_lock()` (which guards against exactly this anti-pattern) or cause a hard crash. Monitor the kernel log for BUG, WARN, or oops traces. If a warning or crash is detected, call `kstep_fail("forceidle balance callback fired from rt_mutex_setprio context")`.
   - **Secondary detection (callback state inspection, read-only)**: If the crash is hard to trigger deterministically, use the `on_tick_end` callback to inspect the balance callback list on CPU 3's rq. Read `per_cpu(core_balance_head, 3)` (importing it via `KSYM_IMPORT(core_balance_head)` if needed as a per-CPU variable) and check whether its `.next` field is non-NULL — indicating it has been queued. On the buggy kernel, this callback will be queued after *every* idle-task selection in `set_next_task_idle()`, regardless of whether `core_forceidle_count > 0`. On the fixed kernel, the callback is only queued from `pick_next_task()` with the precise guard `rq->core->core_forceidle_count && next == rq->idle`, and is always consumed within the same `__schedule()` invocation.
   - **Tertiary detection (observation of queue_core_balance call site)**: Read `cpu_rq(3)->core->core_forceidle_count` after idle selection. On the buggy kernel, `queue_core_balance()` fires even when `core_forceidle_count == 0` (i.e., the CPU is genuinely idle, not forced idle). On the fixed kernel, the callback is only queued when `core_forceidle_count > 0`. Detecting a queued `core_balance_head` when `core_forceidle_count == 0` is a definitive sign of the bug.
   - On the **fixed kernel**: `queue_core_balance()` is never called from `set_next_task_idle()`. It is only called from `pick_next_task()` in `core.c` with the condition `rq->core->core_forceidle_count && next == rq->idle`. No stale callbacks can accumulate for `rt_mutex_setprio()` to consume. No WARN or crash occurs. Call `kstep_pass("forceidle balance callback correctly gated in pick_next_task")`.

### Key Implementation Details

- **Core cookie assignment via kernel API**: The entire core-cookie lifecycle is managed through `sched_core_share_pid(PR_SCHED_CORE_CREATE, pid, PIDTYPE_PID, 0)`, imported via `KSYM_IMPORT`. This function is not EXPORT_SYMBOL'd, but KSYM_IMPORT resolves symbols from kallsyms, so it can import any non-inlined kernel function regardless of export status. Each `PR_SCHED_CORE_CREATE` call allocates a new unique cookie and assigns it through the full kernel path, including rq locking, core-tree management, `sched_core_get()` refcounting, and rescheduling. This completely eliminates direct writes to `core_cookie`, manual `sched_core_enqueue()` calls, and manual `sched_core_get()` calls.
- **ptrace_may_access() from kthread context**: The `sched_core_share_pid()` function checks `ptrace_may_access(task, PTRACE_MODE_READ_REALCREDS)`. Since the kSTEP driver runs as a kernel thread with full capabilities (effectively root), this check will pass for any target task. The PID lookup via `find_task_by_vpid()` also works correctly because kthreads reside in the init PID namespace.
- **rt_mutex from kernel module**: The `rt_mutex` API (`rt_mutex_init()`, `rt_mutex_lock()`, `rt_mutex_unlock()`) is available in kernel modules. Define a static `DEFINE_RT_MUTEX(test_mutex)` and have kthreads contend on it. No KSYM_IMPORT is needed for rt_mutex operations.
- **Read-only internal access**: The driver reads `task_struct->core_cookie`, `rq->core->core_forceidle_count`, `rq->core->core_cookie`, `rq->balance_callback`, and `per_cpu(core_balance_head, cpu)` purely for observation and pass/fail determination. No internal scheduler state is written by the driver.
- **KSTEP_EXTRA_CONFIG**: Create a config fragment file with `CONFIG_SCHED_CORE=y` and pass it via `KSTEP_EXTRA_CONFIG` when building the kernel.
- **Guard with LINUX_VERSION_CODE**: Use `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && LINUX_VERSION_CODE < KERNEL_VERSION(5,18,0)` to guard the driver, since the bug exists from v5.14-rc1 (when core scheduling and the forceidle balancer were introduced) through v5.18-rc1.

### Expected Behavior

- **Buggy kernel (pre-v5.18-rc2)**: The forceidle balance callback is queued every time the idle task is selected via `set_next_task_idle()`, regardless of whether the CPU is genuinely forced idle. When `rt_mutex_setprio()` subsequently calls `balance_callback()`, it finds and executes the stale `core_balance_head` callback, running `sched_core_balance()` from the wrong rq->lock context. This triggers a WARN from `rq_pin_lock()` or a crash. The driver detects this via the crash/WARN or by observing the inappropriate callback queuing through read-only inspection of the balance callback list.
- **Fixed kernel (v5.18-rc2+)**: The `queue_core_balance()` call is only made from `pick_next_task()` in `core.c` with the precise condition `rq->core->core_forceidle_count && next == rq->idle`. The callback is always consumed within the same `__schedule()` invocation's `balance_callback()` call. No stale callbacks exist for `rt_mutex_setprio()` to pick up. No WARN or crash occurs.
