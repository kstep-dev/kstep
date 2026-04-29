# Core: ttwu wakelist bypasses cpus_mask check

**Commit:** `751d4cbc43879229dbc124afefe240b70fd29a85`
**Affected files:** kernel/sched/core.c
**Fixed in:** v6.0-rc1
**Buggy since:** v5.8-rc1 (introduced by `c6e7bd7afaeb` "sched/core: Optimize ttwu() spinning on p->on_cpu")

## Bug Description

The `try_to_wake_up()` (ttwu) fast path introduced in commit `c6e7bd7afaeb` optimizes wakeups by avoiding the spin-wait on `p->on_cpu` when the target task is still descheduling. When a waker CPU observes `p->on_cpu == 1` (meaning the task is in the middle of a context switch on its current CPU), instead of spinning until the context switch completes, it queues the wakeup on the target CPU's `wake_list` via an IPI. The IPI is guaranteed to be processed after the context switch completes (since `schedule()` runs with interrupts disabled), so this is safe for forward progress.

However, the optimization in `ttwu_queue_cond()` (originally `ttwu_queue_remote()`) only checks whether the waker and target CPUs share a cache (LLC domain) and whether the target CPU's runqueue is empty. It does **not** check whether the descheduling task is still allowed to run on its current CPU according to its `cpus_ptr` (CPU affinity mask). This means that if a task's affinity was changed to exclude its current CPU (e.g., by `set_cpus_allowed_ptr()`), and the task is in the process of being descheduled, a concurrent wakeup from a CPU in a different LLC domain can re-queue the task on the old (now disallowed) CPU.

The bug was discovered on a large HPE Superdome Flex machine with 440+ CPUs during early boot. A workqueue rescue thread (`mm_percpu_wq`) that was being migrated away from its current CPU was woken up by a parallel udev process running on a CPU in a different LLC domain. The wakeup hit the fast path, queued the rescue thread on the old CPU, and the workqueue code then detected that the per-cpu workqueue worker was running on the wrong CPU, triggering a `WARNING` at `kernel/workqueue.c:2231` in `process_one_work()`.

The root issue is a missing affinity check in the ttwu fast path that allows a task to be enqueued on a CPU that its `cpus_ptr` mask explicitly excludes.

## Root Cause

The bug is in the `ttwu_queue_cond()` function (called from `ttwu_queue_wakelist()`, which is called from `try_to_wake_up()`). In the buggy code, the function signature is:

```c
static inline bool ttwu_queue_cond(int cpu)
```

It only takes a CPU number and checks:
1. `!cpu_active(cpu)` — skip if CPU is in hotplug transition
2. `!cpus_share_cache(smp_processor_id(), cpu)` — if CPUs don't share LLC, queue remotely (returns `true`)
3. `cpu == smp_processor_id()` — don't queue locally
4. `!cpu_rq(cpu)->nr_running` — if target CPU is idle, queue there

Critically, it never examines the task's `p->cpus_ptr` to verify the target CPU is in the task's allowed CPU set.

The call chain is:
1. `try_to_wake_up()` acquires `p->pi_lock`, observes `p->on_rq == 0` (task is dequeued/sleeping), then checks `smp_load_acquire(&p->on_cpu)`.
2. If `p->on_cpu == 1`, the task is still in the middle of `__schedule()` on its current CPU. The code then calls `ttwu_queue_wakelist(p, task_cpu(p), wake_flags)`.
3. `ttwu_queue_wakelist()` calls `ttwu_queue_cond(cpu)` (without the task pointer).
4. If the waker is on a CPU in a different LLC domain from `task_cpu(p)`, `cpus_share_cache()` returns `false`, and `ttwu_queue_cond()` returns `true`.
5. The task is placed on the target CPU's `wake_list` via `__ttwu_queue_wakelist()` → `__smp_call_single_queue()`, which sends an IPI.
6. When the IPI fires on the target CPU (after the context switch completes), `sched_ttwu_pending()` activates the task on that CPU via `ttwu_do_activate()`.

At step 2, `task_cpu(p)` returns the CPU where the task was last running. But if `set_cpus_allowed_ptr()` was called previously, `p->cpus_ptr` may have already been updated to exclude `task_cpu(p)`. The normal (slow) ttwu path would wait for `p->on_cpu` to clear via `smp_cond_load_acquire(&p->on_cpu, !VAL)`, then call `select_task_rq()` which respects `p->cpus_ptr`. The fast path bypasses both the wait and the CPU selection.

The specific race involves three actors:
- **CPU X**: Running task T, which will be descheduled
- **Migration mechanism**: `set_cpus_allowed_ptr()` has already updated `T->cpus_ptr` to exclude CPU X, and queued a stopper thread migration request
- **CPU Z** (different LLC from CPU X): A parallel wakeup source calling `try_to_wake_up(T)`

The race window is between when `T->on_rq` becomes 0 (task dequeued in `__schedule()`) and when `T->on_cpu` becomes 0 (in `finish_task()` after the context switch). This window covers the entire duration of the context switch (register save/restore, stack switch), which is on the order of microseconds.

## Consequence

The task is enqueued and runs on a CPU that is explicitly excluded from its `cpus_ptr` affinity mask. This violates a fundamental kernel invariant: a task must only run on CPUs allowed by its affinity mask.

The concrete consequence observed was a `WARNING` in the workqueue subsystem at `kernel/workqueue.c:2231` in `process_one_work()`. This function contains an assertion (`WARN_ON_ONCE`) that checks whether a per-CPU workqueue worker is running on the correct CPU. The workqueue rescue thread (`mm_percpu_wq`) was found running on the wrong CPU, triggering:

```
WARNING: CPU: 439 PID: 10 at ../kernel/workqueue.c:2231 process_one_work+0x4d/0x440
Call Trace:
 <TASK>
 rescuer_thread+0x1f6/0x360
 kthread+0x156/0x180
 ret_from_fork+0x22/0x30
 </TASK>
```

Beyond the workqueue warning, running a task on a disallowed CPU can cause broader issues: data corruption in per-CPU data structures that assume CPU-local access, violation of cpuset constraints in cgroup-controlled environments, incorrect NUMA placement leading to performance degradation, and potential security violations in environments that use CPU affinity for isolation (e.g., running a task on a CPU allocated to a different security domain). Any kernel or userspace code that relies on CPU affinity guarantees (via `sched_setaffinity()`, cpusets, or `kthread_bind()`) is potentially affected.

The bug is more likely to manifest on large machines with complex NUMA topologies where many LLC domains exist, increasing the chance that a waker and the target task reside in different LLC domains. The race is also more likely during boot when many kthreads and workqueues are being initialized and migrated simultaneously.

## Fix Summary

The fix modifies `ttwu_queue_cond()` to accept the task pointer and adds a `cpumask_test_cpu()` check:

```c
-static inline bool ttwu_queue_cond(int cpu)
+static inline bool ttwu_queue_cond(struct task_struct *p, int cpu)
 {
     if (!cpu_active(cpu))
         return false;

+    /* Ensure the task will still be allowed to run on the CPU. */
+    if (!cpumask_test_cpu(cpu, p->cpus_ptr))
+        return false;
+
     if (!cpus_share_cache(smp_processor_id(), cpu))
         return true;
     ...
```

The caller `ttwu_queue_wakelist()` is updated to pass the task pointer:

```c
-    if (sched_feat(TTWU_QUEUE) && ttwu_queue_cond(cpu)) {
+    if (sched_feat(TTWU_QUEUE) && ttwu_queue_cond(p, cpu)) {
```

When the task's `cpus_ptr` does not include the current CPU (`task_cpu(p)`), `ttwu_queue_cond()` returns `false`. This prevents the fast path from queuing the task on the disallowed CPU. Control falls through to the slow path: `smp_cond_load_acquire(&p->on_cpu, !VAL)` waits for the context switch to complete, then `select_task_rq()` is called which properly selects a CPU from the task's allowed set. This fix is correct because `p->cpus_ptr` is stabilized by `p->pi_lock`, which is held by `try_to_wake_up()` at this point, so the check is safe from concurrent affinity changes.

The fix is minimal and precisely targeted: it adds a single `cpumask_test_cpu()` check to the fast path condition, gracefully falling back to the normal (correct) wakeup path when the fast path would violate affinity constraints. The performance impact is negligible — one extra bitmask test in a function that already performs multiple checks.

## Triggering Conditions

1. **Multi-CPU system with multiple LLC domains**: At minimum, two CPUs that do not share an LLC (last-level cache) are required. The function `cpus_share_cache()` uses `per_cpu(sd_llc_id, ...)` to determine this, which is derived from the `SD_SHARE_PKG_RESOURCES` flag in sched domain topology. In practice, this means CPUs on different packages/sockets, or different MC-level groups depending on the platform.

2. **CPU affinity change**: A task's `cpus_ptr` must be changed to exclude its current CPU. This happens via `set_cpus_allowed_ptr()`, `sched_setaffinity()`, cpuset updates, or workqueue-internal affinity management (e.g., workqueue rescue threads that need to run on specific CPUs).

3. **Task descheduling**: The task whose affinity was changed must enter `__schedule()` (go to sleep or be preempted) on the old (now disallowed) CPU. During the context switch, `p->on_cpu` is 1 and `p->on_rq` is 0.

4. **Concurrent wakeup from different LLC**: While the task is descheduling (on_cpu=1, on_rq=0), a wakeup for that task must arrive from a CPU in a different LLC domain. The waker must observe `p->on_rq == 0` and `p->on_cpu == 1` in `try_to_wake_up()`.

5. **TTWU_QUEUE sched feature enabled**: The `TTWU_QUEUE` scheduler feature must be active (it is enabled by default in all standard kernel configurations).

6. **Race window**: The wakeup must occur during the window between the task being dequeued (`p->on_rq = 0`) and the context switch completing (`p->on_cpu = 0` in `finish_task()`). This window is typically a few microseconds (covering register save/restore and stack switch). The probability of hitting this window increases with: more CPUs (more potential wakers), complex topology (more LLC boundaries), and heavy scheduling activity (more wakeups per unit time).

The original report was on a 440+ CPU HPE Superdome Flex during early boot, where workqueue rescue threads were being migrated and udev was generating parallel wakeups. The bug is probabilistic but becomes increasingly likely on larger machines with more NUMA nodes.

## Reproduce Strategy (kSTEP)

### Overview

The bug requires a race between a task descheduling on one CPU and a wakeup arriving from a CPU in a different LLC domain. The core challenge for kSTEP is creating this temporal overlap, since the driver runs sequentially on CPU 0. The on_cpu=1 window during context switch is brief (microseconds), but can be targeted with a concurrent spinning waker.

### Topology Setup

Configure at least 3 CPUs (0, 1, 2) with CPUs 1 and 2 in **different LLC (MC-level) domains**:

```c
kstep_topo_init();
const char *mc_groups[] = {"0-1", "2"};
kstep_topo_set_mc(mc_groups, 2);
kstep_topo_apply();
```

This ensures `cpus_share_cache(1, 2)` returns `false`, which is required to trigger the `ttwu_queue_cond()` fast path.

### Task Creation

Create two kthreads:
1. **Task T** (the victim): Bound to CPU 1 initially. Its affinity will later be changed to only CPU 2.
2. **Task W** (the waker): Bound to CPU 2. Will repeatedly attempt to wake T.

```c
struct task_struct *T = kstep_kthread_create("victim");
struct task_struct *W = kstep_kthread_create("waker");
cpumask_t mask1, mask2;
cpumask_clear(&mask1); cpumask_set_cpu(1, &mask1);
cpumask_clear(&mask2); cpumask_set_cpu(2, &mask2);
kstep_kthread_bind(T, &mask1);
kstep_kthread_bind(W, &mask2);
kstep_kthread_start(T);
kstep_kthread_start(W);
```

### Triggering the Race

The key sequence:

1. **Change T's affinity to exclude CPU 1**: Use `KSYM_IMPORT(set_cpus_allowed_ptr)` to call `set_cpus_allowed_ptr(T, &mask2)`. This updates `T->cpus_ptr` to only allow CPU 2 but T may still be on CPU 1 (a migration via the stopper thread is pending).

2. **Block T**: Use `kstep_kthread_block(T)` to make T go to sleep on CPU 1. This triggers `__schedule()` on CPU 1 where `T->on_rq` becomes 0 and `T->on_cpu` is 1 during the context switch.

3. **Wake T from CPU 2**: Use `kstep_kthread_syncwake(W, T)` to have W (on CPU 2, different LLC) call `wake_up_process(T)`.

The race window is between steps 2 and 3. Because `kstep_kthread_block()` may be synchronous (returning only after T fully descheduled), the simplest approach may not reliably hit the window. Two strategies to address this:

**Strategy A — Repeated rapid cycling**: In a loop (e.g., 1000 iterations), rapidly alternate between waking T on CPU 1, changing its affinity to CPU 2, blocking T, and having W wake T. The hope is that scheduling jitter causes the wakeup to occasionally overlap with the on_cpu=1 window.

**Strategy B — Custom concurrent waker (kSTEP extension)**: Add a small kSTEP helper (e.g., `kstep_kthread_wake_loop(waker, target, count)`) that makes the waker kthread spin-call `wake_up_process(target)` in a tight loop for `count` iterations. Meanwhile, the driver triggers T to block. The spinning waker on CPU 2 will continuously probe `try_to_wake_up(T)`, and one attempt may catch the on_cpu=1 window during T's context switch.

### Detection

After each wakeup, check whether the bug was triggered:

```c
int actual_cpu = task_cpu(T);
if (!cpumask_test_cpu(actual_cpu, T->cpus_ptr)) {
    kstep_fail("Task on disallowed CPU %d (allowed: %*pbl)",
               actual_cpu, cpumask_pr_args(T->cpus_ptr));
} else {
    kstep_pass("Task correctly on CPU %d", actual_cpu);
}
```

On the **buggy kernel**: After hitting the race, `task_cpu(T)` will be 1 (the old CPU), but `T->cpus_ptr` only includes CPU 2. The check detects the violation.

On the **fixed kernel**: `ttwu_queue_cond()` returns `false` when `cpumask_test_cpu(cpu, p->cpus_ptr)` fails, so the wakeup falls through to `select_task_rq()` which properly places T on CPU 2. The check always passes.

### kSTEP Extensions Needed

- **`KSYM_IMPORT(set_cpus_allowed_ptr)`**: To change a running task's CPU affinity mask at runtime. This is a straightforward symbol import.
- **Concurrent waker helper** (optional, for Strategy B): A new `kstep_kthread_wake_loop()` or similar function that makes a kthread repeatedly call `wake_up_process()` on a target. This is a minor extension (~20 lines) to `kmod/kthread.c`. Alternatively, the driver could directly schedule a work item on CPU 2 that spins on the wakeup.

### Expected Behavior

- **Buggy kernel (pre-fix)**: After a sufficient number of iterations, T should be found on CPU 1 despite its affinity only allowing CPU 2. The probability per iteration depends on timing; with the spinning waker approach (Strategy B), it should trigger within 100-1000 iterations.
- **Fixed kernel**: T is always placed on CPU 2 (or another allowed CPU) after wakeup, regardless of timing. The `cpumask_test_cpu()` check in `ttwu_queue_cond()` prevents the fast path from queuing on a disallowed CPU.

### Configuration

QEMU should be configured with at least 3 CPUs (`-smp 3`). The `TTWU_QUEUE` sched feature must be enabled (it is by default). No special kernel config options are needed beyond standard SMP support.
