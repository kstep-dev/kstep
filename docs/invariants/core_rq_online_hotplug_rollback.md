# Active CPU Runqueue Online Consistency
**Source bug:** `fe7a11c78d2a9bdb8b50afc278a31ac177000948`

**Property:** For any CPU present in `cpu_active_mask`, its runqueue must have `rq->online == 1` (and the CPU must be set in `rq->rd->online`).

**Variables:**
- `cpu_is_active` — whether the CPU is set in `cpu_active_mask`. Read in-place via `cpu_active(cpu)`.
- `rq_online` — the `rq->online` field for that CPU's runqueue. Read in-place via `cpu_rq(cpu)->online`.
- `rd_online` — whether the CPU is set in `rq->rd->online`. Read in-place via `cpumask_test_cpu(cpu, rq->rd->online)`.

**Check(s):**

Check 1: Performed at `scheduler_tick()`. For the current CPU, which must be active.
```c
struct rq *rq = this_rq();
int cpu = smp_processor_id();

// A CPU executing scheduler_tick() is by definition active.
// Its rq must reflect that.
if (WARN_ON_ONCE(!rq->online))
    pr_err("sched: CPU %d is active but rq->online == 0\n", cpu);
if (rq->rd && WARN_ON_ONCE(!cpumask_test_cpu(cpu, rq->rd->online)))
    pr_err("sched: CPU %d is active but not in rd->online\n", cpu);
```

Check 2: Performed at `sched_cpu_deactivate()` return point (both success and failure paths). After the function returns, the postcondition must hold.
```c
// At the end of sched_cpu_deactivate(), on the error (rollback) path:
// cpu_active_mask has been restored => cpu is active => rq->online must be 1
if (ret) {
    // After rollback completes:
    WARN_ON_ONCE(!rq->online);
    WARN_ON_ONCE(rq->rd && !cpumask_test_cpu(cpu, rq->rd->online));
}
```

**Example violation:** When `cpuset_cpu_inactive()` fails during CPU deactivation, the error path restores `cpu_active_mask` but does not call `sched_set_rq_online()`, leaving `rq->online == 0` for an active CPU. Check 1 would fire on the next scheduler tick on that CPU.

**Other bugs caught:** None known, but this would catch any future bug where hotplug rollback or activation/deactivation paths leave `rq->online` inconsistent with `cpu_active_mask`.
