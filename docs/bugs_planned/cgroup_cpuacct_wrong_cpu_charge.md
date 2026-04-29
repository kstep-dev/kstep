# Cgroup: cpuacct percpu cpuusage charged to wrong CPU during load_balance

**Commit:** `248cc9993d1cc12b8e9ed716cc3fc09f6c3517dd`
**Affected files:** kernel/sched/cpuacct.c
**Fixed in:** v5.18-rc1
**Buggy since:** v4.6-rc1 (commit `73e6aafd9ea8` "sched/cpuacct: Simplify the cpuacct code")

## Bug Description

The `cpuacct` cgroup controller tracks per-CPU CPU usage for tasks in each cgroup. It records how much CPU time was consumed by tasks in a given cgroup on each individual CPU via the `cpuacct.usage_percpu` interface. This per-CPU breakdown is used by system administrators and container runtimes to understand which CPUs are executing a cgroup's workload and to detect scheduling asymmetries or imbalances.

The bug causes CPU time to be incorrectly attributed to the wrong CPU in the per-CPU cpuacct accounting. When `cpuacct_charge()` is called during load balancing, it uses `__this_cpu_add()` to increment the cpuusage counter. However, during load balancing, the code runs on the CPU performing the balance (e.g., CPU 0) while operating on the runqueue of a remote CPU (e.g., CPU 1). The `__this_cpu_add()` macro always operates on the current CPU's percpu variable, so the time that should be attributed to CPU 1 (where the task actually ran) is instead attributed to CPU 0 (where the load balancer happens to be executing).

This bug was introduced in commit `73e6aafd9ea8` which simplified the cpuacct code by converting `per_cpu_ptr(ca->cpuusage, cpu)` to `this_cpu_ptr(ca->cpuusage)` (equivalent to `__this_cpu_add`). The simplification was incorrect because it assumed `cpuacct_charge()` always runs on the same CPU as the task being charged. While this assumption holds for most callers (e.g., `scheduler_tick()` → `task_tick_fair()` → `update_curr()`), it does not hold when `update_curr()` is called from the load balancing path, where one CPU manipulates another CPU's runqueue.

## Root Cause

The `cpuacct_charge()` function (in `kernel/sched/cpuacct.c`) is responsible for charging a task's execution time to the cpuacct hierarchy. In the buggy code, it uses `__this_cpu_add(*ca->cpuusage, cputime)` to increment the per-CPU usage counter:

```c
void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
    struct cpuacct *ca;
    rcu_read_lock();
    for (ca = task_ca(tsk); ca; ca = parent_ca(ca))
        __this_cpu_add(*ca->cpuusage, cputime);  // BUG: uses current CPU, not task's CPU
    rcu_read_unlock();
}
```

The `__this_cpu_add()` macro expands to an addition on the percpu variable slot for `smp_processor_id()` — the CPU currently executing the code. This is correct when the function is called in contexts where the current CPU is the same as the task's CPU, such as:

- `scheduler_tick()` → `task_tick_fair()` → `entity_tick()` → `update_curr()`: The tick fires on the CPU running the task.
- `put_prev_task_fair()` → `put_prev_entity()` → `update_curr()`: Called during context switch on the task's CPU.

However, `cpuacct_charge()` is also called through the following cross-CPU path during load balancing:

1. `load_balance()` executes on CPU 0 (the idle or lightly loaded CPU).
2. `find_busiest_queue()` identifies CPU 1 as the busiest.
3. `detach_tasks()` iterates through CPU 1's cfs_tasks list while holding CPU 1's rq lock.
4. For each migratable task, `detach_task()` → `deactivate_task()` → `dequeue_task()` → `dequeue_task_fair()` is called.
5. `dequeue_task_fair()` → `dequeue_entity()` calls `update_curr(cfs_rq)` at line 4392 of fair.c (v5.17).
6. `update_curr()` computes the delta execution time for `cfs_rq->curr` (the currently running entity on CPU 1's CFS runqueue) and calls `cgroup_account_cputime(curtask, delta_exec)` at line 877.
7. `cgroup_account_cputime()` → `cpuacct_charge(task, delta_exec)` runs on CPU 0 but for a task whose `task_cpu()` is CPU 1.
8. `__this_cpu_add(*ca->cpuusage, cputime)` increments `ca->cpuusage` for CPU 0 instead of CPU 1.

The root cause is the incorrect assumption that `smp_processor_id() == task_cpu(tsk)` always holds when `cpuacct_charge()` is called. This invariant is violated whenever `update_curr()` is invoked from a cross-CPU code path, most notably during CFS load balancing.

## Consequence

The observable consequence is incorrect per-CPU cpuacct statistics visible through the `cpuacct.usage_percpu`, `cpuacct.usage_percpu_user`, and `cpuacct.usage_percpu_sys` cgroup interfaces. CPU time that a task actually consumed on one CPU is incorrectly attributed to a different CPU — specifically, whichever CPU happened to run `load_balance()`.

For container orchestration systems (Kubernetes, Docker) and monitoring tools that rely on per-CPU cpuacct data to detect scheduling anomalies, perform NUMA-aware scheduling decisions, or bill for CPU usage, this bug causes inaccurate metrics. CPUs that perform load balancing will appear to have inflated usage, while CPUs whose tasks are being balanced away will appear to have deflated usage. This can lead to incorrect capacity planning decisions and misleading monitoring dashboards.

The bug does not cause crashes, hangs, or data corruption — it is purely a statistics-accounting error. However, the misattribution grows over time as load balancing occurs repeatedly. On busy systems where load balancing is frequent (multiple CPUs, fluctuating workloads), the cumulative error in per-CPU usage statistics can become substantial. The total cgroup CPU usage (`cpuacct.usage`) remains correct since the same amount of time is charged; only its per-CPU distribution is wrong.

## Fix Summary

The fix in commit `248cc9993d1cc12b8e9ed716cc3fc09f6c3517dd` changes `cpuacct_charge()` to use `task_cpu(tsk)` to determine the correct CPU for the percpu counter update, instead of relying on the current CPU via `__this_cpu_add()`:

```c
void cpuacct_charge(struct task_struct *tsk, u64 cputime)
{
    unsigned int cpu = task_cpu(tsk);  // NEW: get the task's actual CPU
    struct cpuacct *ca;
    rcu_read_lock();
    for (ca = task_ca(tsk); ca; ca = parent_ca(ca))
        *per_cpu_ptr(ca->cpuusage, cpu) += cputime;  // FIX: charge to task's CPU
    rcu_read_unlock();
}
```

The fix replaces `__this_cpu_add(*ca->cpuusage, cputime)` with `*per_cpu_ptr(ca->cpuusage, cpu) += cputime` where `cpu = task_cpu(tsk)`. The `per_cpu_ptr()` macro explicitly selects the percpu variable slot for the specified CPU, ensuring the charge always goes to the CPU where the task is (or was) running, regardless of which CPU executes `cpuacct_charge()`.

Note that the companion function `cpuacct_account_field()` is NOT changed because it is only called from `task_group_account_field()` in the tick accounting path (`cputime.c`), which always runs on the current task's own CPU. The fix is correct and complete because `cpuacct_charge()` is the only cpuacct function that can be called from a cross-CPU context. The accompanying patches in the v3 series (patches 2/3 and 3/3) also replace the RCU read lock in `cpuacct_charge()` with `lockdep_assert_rq_held()` since the function always runs with the task's rq lock held, and remove redundant RCU locks from `cpuacct_account_field()`.

## Triggering Conditions

The bug triggers whenever `load_balance()` on CPU A dequeues (migrates) a CFS task from CPU B's runqueue. The specific conditions are:

1. **Multi-CPU system (≥ 2 CPUs):** The system must have at least 2 CPUs so that load balancing between CPUs can occur. More CPUs increase the frequency of load balancing.

2. **Load imbalance:** There must be a load imbalance between CPUs, with at least one CPU having significantly more runnable CFS tasks than another. This causes `find_busiest_group()` and `find_busiest_queue()` to identify a busiest CPU and attempt to pull tasks.

3. **Migratable tasks:** At least one task on the busiest CPU must be eligible for migration (i.e., its `cpus_allowed` mask includes the destination CPU, and `can_migrate_task()` returns true). If all tasks are pinned, `detach_tasks()` won't call `detach_task()` and `update_curr()` won't be triggered from this path.

4. **CONFIG_CGROUP_CPUACCT=y:** The kernel must be compiled with cpuacct support. This is enabled by default on most distribution kernels.

5. **Task has accumulated runtime:** `update_curr()` must compute a positive `delta_exec` for `cfs_rq->curr` on the source CPU. This means the currently running task on the source CPU must have been running for some nonzero time since its last `exec_start` update.

The bug is highly reliable and deterministic — it triggers every time load balancing migrates a task across CPUs and `update_curr()` runs for a remote CPU's cfs_rq. On a busy system with multiple CPUs and fluctuating loads, this happens frequently (potentially every load balance interval, typically every 4ms–64ms depending on the sched domain level).

There are no race conditions or timing requirements beyond the normal load balancing trigger. The bug is purely a logic error in per-CPU variable selection, not a concurrency issue.

## Reproduce Strategy (kSTEP)

The strategy is to trigger CFS load balancing between two CPUs, observe `cpuacct_charge()` charging the per-CPU usage counter, and verify whether the charge goes to the correct CPU. Here is a concrete step-by-step plan:

### Setup

1. **QEMU configuration:** 2 CPUs, standard RAM. No special topology needed.
2. **Kernel version guard:** `#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && LINUX_VERSION_CODE < KERNEL_VERSION(5, 18, 0)` for the buggy range, though the driver should also work on v5.18+ to confirm the fix.

### Data Structures

Duplicate the `struct cpuacct` definition in the driver (it is defined in `kernel/sched/cpuacct.c` and not exposed in any header):

```c
struct cpuacct {
    struct cgroup_subsys_state css;
    u64 __percpu *cpuusage;
    struct kernel_cpustat __percpu *cpustat;
};
```

Create a helper to read cpuacct per-CPU usage for a given task:

```c
static u64 read_cpuacct_usage(struct task_struct *tsk, int cpu)
{
    struct cgroup_subsys_state *css = task_css(tsk, cpuacct_cgrp_id);
    struct cpuacct *ca = container_of(css, struct cpuacct, css);
    return *per_cpu_ptr(ca->cpuusage, cpu);
}
```

This accesses the root cpuacct (or whichever cpuacct the task belongs to) and reads the percpu `cpuusage` for the specified CPU. The `cpuacct_cgrp_id` enum is available from `<linux/cgroup-defs.h>`, and `task_css()` is available from `<linux/cgroup.h>`, both of which are included by `kernel/sched/sched.h` (accessible through kSTEP's `internal.h`).

### Task Creation (in `run()`)

1. **Create task A** (will be `cfs_rq->curr` on CPU 1): `kstep_task_create()`, pin to CPU 1 with `kstep_task_pin(taskA, 1, 2)`. This task stays on CPU 1 and cannot be migrated.
2. **Create tasks B, C, D** (migratable, create imbalance): `kstep_task_create()` for each, then pin them temporarily to CPU 1 with `kstep_task_pin(taskX, 1, 2)` initially. Before triggering load balance, unpin one of them (make cpus_allowed = all CPUs) so it becomes migratable.
3. Actually, for simplicity: create 3-4 tasks without pinning. Use `kstep_task_pin()` with range `(1, 2)` for all initially. Then before triggering load balance, unpin one task by calling `kstep_task_pin(taskB, 0, 2)` to allow it on both CPU 0 and CPU 1.

### Execution Sequence

1. **Let tasks run on CPU 1:** Call `kstep_tick_repeat(5)` to let all tasks on CPU 1 accumulate runtime and establish `exec_start` timestamps.

2. **Unpin one task:** Call `kstep_task_pin(taskB, 0, 2)` so task B becomes migratable to CPU 0.

3. **Record cpuacct "before" values:** Use the `on_sched_balance_begin` callback to record per-CPU cpuacct usage just before load_balance executes:
   ```c
   static u64 cpu0_usage_before, cpu1_usage_before;
   static bool recorded = false;

   void on_balance_begin(int cpu, struct sched_domain *sd) {
       if (cpu == 0 && !recorded) {
           cpu0_usage_before = read_cpuacct_usage(taskA, 0);
           cpu1_usage_before = read_cpuacct_usage(taskA, 1);
           recorded = true;
       }
   }
   ```

4. **Trigger load balance:** Call `kstep_tick()` with CPU 0 idle and CPU 1 overloaded. The scheduler softirq on CPU 0 should invoke `load_balance()`, which identifies CPU 1 as busiest, locks CPU 1's rq, and calls `detach_tasks()`. During `detach_tasks()`, `dequeue_entity()` → `update_curr()` is called for `cfs_rq->curr` on CPU 1 (task A or whichever task is currently running). This triggers `cpuacct_charge()`.

5. **Record cpuacct "after" values:** After the tick completes, read per-CPU cpuacct usage again:
   ```c
   u64 cpu0_usage_after = read_cpuacct_usage(taskA, 0);
   u64 cpu1_usage_after = read_cpuacct_usage(taskA, 1);
   u64 cpu0_delta = cpu0_usage_after - cpu0_usage_before;
   u64 cpu1_delta = cpu1_usage_after - cpu1_usage_before;
   ```

### Detection Criteria

- **Buggy kernel (before fix):** `cpu0_delta > 0` — CPU 0's cpuacct usage increased even though no CFS task was running on CPU 0. The `update_curr()` call during `detach_tasks()` on CPU 0 charged the runtime of CPU 1's current task to CPU 0's percpu slot via `__this_cpu_add()`. Report `kstep_fail("cpuacct charged to wrong CPU: cpu0_delta=%llu, cpu1_delta=%llu", cpu0_delta, cpu1_delta)`.

- **Fixed kernel (after fix):** `cpu0_delta == 0` and `cpu1_delta > 0` — all cpuacct charges from CPU 1's tasks correctly go to CPU 1's percpu slot via `per_cpu_ptr(ca->cpuusage, task_cpu(tsk))`. Report `kstep_pass("cpuacct correctly charged to task CPU: cpu0_delta=%llu, cpu1_delta=%llu", cpu0_delta, cpu1_delta)`.

### Important Notes

- The `on_sched_balance_begin` callback runs inside the scheduler softirq on CPU 0, before load_balance dequeues any tasks. This is the ideal point to snapshot cpuacct values because no CFS `update_curr()` runs on CPU 0 between `on_sched_balance_begin` and the `detach_tasks()` call. Thus, any increase in CPU 0's root cpuacct during this window can only come from the bug (cross-CPU charge from `update_curr()` on CPU 1's cfs_rq).

- Since we're reading the root cpuacct (all tasks are in it by default), the measurement includes charges from all tasks. To get a clean signal, ensure no CFS tasks are runnable on CPU 0 (other than possibly the idle task, which doesn't get CFS charges). The driver thread runs on CPU 0, but its CFS charges happen during `scheduler_tick()` before load_balance, not during the load_balance window itself.

- The driver should log all relevant values (cpu0_usage_before, cpu0_usage_after, cpu1_usage_before, cpu1_usage_after, deltas) for debugging.

- If load_balance doesn't trigger on the first tick (e.g., the imbalance isn't large enough), repeat the tick sequence. Use `kstep_tick_repeat()` or a loop with `kstep_tick()` checking whether `recorded` was set in the callback.

- As an alternative detection method, one could use `register_kprobe()` on `cpuacct_charge` and check `smp_processor_id() != task_cpu(tsk)` in the pre-handler. If detected, read `per_cpu_ptr(ca->cpuusage, smp_processor_id())` before and after to confirm the incorrect CPU was charged. This is more invasive but gives direct proof of the mismatch.
