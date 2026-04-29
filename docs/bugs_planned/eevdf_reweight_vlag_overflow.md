# EEVDF: Unbounded vlag in reweight_eevdf() causes s64 overflow and NULL deref

**Commit:** `1560d1f6eb6b398bddd80c16676776c0325fe5fe`
**Affected files:** `kernel/sched/fair.c`
**Fixed in:** v6.9-rc6
**Buggy since:** v6.7-rc2 (introduced by `eab03c23c2a1` "sched/eevdf: Fix vruntime adjustment on reweight")

## Bug Description

The `reweight_eevdf()` function in the CFS/EEVDF scheduler computes the virtual lag (`vlag`) of a scheduling entity without applying the bounds that `update_entity_lag()` enforces. When a group scheduling entity (created by cgroup hierarchies via `CONFIG_FAIR_GROUP_SCHED`) undergoes a dramatic weight reduction through `update_cfs_group()` → `reweight_entity()` → `reweight_eevdf()`, the unbounded vlag is fed into a scaling multiplication `vlag * old_weight` that overflows the s64 type. This produces a wildly incorrect `se->vruntime`, which in turn causes `entity_eligible()` to give false-negative results for every entity on the CFS runqueue. When no entity is deemed eligible, `pick_eevdf()` returns NULL, and the scheduler crashes with a NULL pointer dereference in `pick_next_task_fair()` or `set_next_entity()`.

The bug was introduced in commit `eab03c23c2a1` ("sched/eevdf: Fix vruntime adjustment on reweight"), which added the `reweight_eevdf()` function to correctly adjust entity vruntimes when their weight changes. That commit implemented the correct mathematical formula `v' = V - (V - v) * w / w'` (equation 4 in the code comments) but failed to clamp the intermediate value `(V - v)` — the entity's lag — to the same bounds that `update_entity_lag()` uses. The EEVDF scheduler's design constrains lag to the range `[-limit, limit]` where `limit = calc_delta_fair(max(2 * slice, TICK_NSEC), se)`, approximately double the entity's time slice. Without this constraint in the reweight path, the lag can grow without bound.

The bug is rare but fatal when it occurs. Multiple independent reporters encountered it on machines running nightly kernel builds (high task churn with many short-lived compile processes), on Android systems with frequently changing nice values, and through kernel fuzzing with the Trinity syscall fuzzer. The crash manifests as `BUG: kernel NULL pointer dereference, address: 00000000000000a0` at `pick_next_task_fair+0x89/0x5a0`, where offset 0xa0 corresponds to dereferencing the `rb_left` pointer of the `sched_entity` returned by `pick_eevdf()` which is NULL.

## Root Cause

The root cause is the missing vlag clamping in `reweight_eevdf()`. In the buggy code at line 3762 of `fair.c`:

```c
if (avruntime != se->vruntime) {
    vlag = (s64)(avruntime - se->vruntime);          // UNBOUNDED
    vlag = div_s64(vlag * old_weight, weight);        // OVERFLOW POSSIBLE
    se->vruntime = avruntime - vlag;                  // WILD VRUNTIME
}
```

The function `update_entity_lag()` (which computes vlag for dequeue operations) already contains the critical clamping logic:

```c
lag = avg_vruntime(cfs_rq) - se->vruntime;
limit = calc_delta_fair(max_t(u64, 2*se->slice, TICK_NSEC), se);
se->vlag = clamp(lag, -limit, limit);
```

But `reweight_eevdf()` computes the same `avruntime - se->vruntime` difference without any such clamp. When this raw, unbounded value is then multiplied by `old_weight` in the expression `div_s64(vlag * old_weight, weight)`, the multiplication of two large s64 values overflows the 64-bit signed integer range (±9.22 × 10¹⁸).

The overflow scenario unfolds as follows. On a system with `nohz_full` or suppressed ticks, a CFS task can run for an extended period without preemption. When the tick finally fires, `update_curr()` accounts the entire elapsed wall time as a single `delta_exec`, which gets converted to virtual time via `calc_delta_fair()` and added to the running entity's vruntime. For a group scheduling entity at the root CFS runqueue level, this can push its vruntime far ahead of `avg_vruntime(cfs_rq)`, creating a large negative lag (meaning the entity has received far more service than its fair share).

Concretely, if a group entity runs alone for 100 seconds of virtual time with 10 competing group entities, its vruntime advances by ~100 billion nanoseconds while the average vruntime only moves by ~10 billion. This creates a vlag of approximately -90 billion nanoseconds (-90 × 10⁹). When `update_cfs_group()` recalculates the group entity's weight via `calc_group_shares()` and determines a new weight, `reweight_eevdf()` is called. If the old weight was approximately 105 million (a typical scaled weight for a heavily loaded group), then `vlag * old_weight ≈ -90 × 10⁹ × 105 × 10⁶ ≈ -9.45 × 10¹⁸`, which exceeds `S64_MAX = 9.22 × 10¹⁸` in absolute value and overflows.

The overflowed vlag produces a nonsensical `se->vruntime = avruntime - vlag` value. This corrupted vruntime then propagates to the eligibility check in `vruntime_eligible()`:

```c
return avg >= (s64)(vruntime - cfs_rq->min_vruntime) * load;
```

Here, `(s64)(vruntime - cfs_rq->min_vruntime)` is an enormous value (because the corrupted vruntime is far from `min_vruntime`), and multiplying it by `load` (the total weight of all entities) causes a second overflow. This second overflow flips the sign of the right-hand side, making the comparison `avg >= (overflowed value)` return `false` even for entities that should be eligible. When `entity_eligible()` returns `false` for every entity in the RB-tree, `pick_eevdf()` exhausts all candidates and returns `NULL`.

## Consequence

The immediate consequence is a kernel NULL pointer dereference (Oops) that typically kills the current process or panics the system. The crash occurs at `pick_next_task_fair+0x89/0x5a0` when the scheduler attempts to dereference the NULL pointer returned by `pick_eevdf()`. The faulting address is consistently `0x00000000000000a0` on x86_64 (or `0x0000002c` on i386), which corresponds to accessing a field at a fixed offset within the `sched_entity` structure from a NULL base pointer.

The crash traces from real-world reports show the Oops occurring in various scheduling contexts:

1. **Process exit path:** `do_exit()` → `do_task_dead()` → `__schedule()` → `pick_next_task_fair()` — This was the most common trigger on Sergei Trofimovich's build machines (processes like `as` and `strip` during nightly kernel compilation).

2. **Timer interrupt return:** `asm_sysvec_apic_timer_interrupt` → `irqentry_exit_to_user_mode()` → `schedule()` → `__schedule()` → `pick_next_task_fair()` — Occurring when the scheduler is invoked after a timer interrupt.

3. **Sleep system call:** `clock_nanosleep()` → `do_nanosleep()` → `schedule()` → `__schedule()` → `pick_next_task_fair()` — The kernel test robot's Trinity fuzzer triggered this path.

The bug was reported on kernel versions 6.8.2 and 6.9.0-rc3, and the kernel test robot reproduced it on 6.7.0-rc1 (the commit that introduced the buggy rbtree sort `2227a957e1d5`, which sits atop the reweight fix `eab03c23c2a1`). The Trinity fuzzer showed a 2.3% reproduction rate (23 out of 999 runs) on a 2-CPU QEMU VM, indicating the bug is rare but non-negligible under heavy scheduling load. The bug is described as "quite rare, but fatal when it does happen" in the commit message.

On Android systems, as noted by the patch author Xuewen Yan, nice values change very frequently, which increases the rate of `reweight_entity()` calls and therefore the probability of triggering the overflow. The author also noted that even after fixing the `on_rq` case, the `!on_rq` case (where `vlag` is scaled in `reweight_entity()` itself) could also exceed limits, though this is a separate issue addressed by a later patch.

## Fix Summary

The fix extracts the vlag clamping logic from `update_entity_lag()` into a new standalone function `entity_lag()`, and then calls this function from both `update_entity_lag()` and `reweight_eevdf()`.

The new `entity_lag()` function computes and clamps the lag in one step:

```c
static s64 entity_lag(u64 avruntime, struct sched_entity *se)
{
    s64 vlag, limit;
    vlag = avruntime - se->vruntime;
    limit = calc_delta_fair(max_t(u64, 2*se->slice, TICK_NSEC), se);
    return clamp(vlag, -limit, limit);
}
```

The `update_entity_lag()` function is simplified to call `entity_lag()`:

```c
static void update_entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
    SCHED_WARN_ON(!se->on_rq);
    se->vlag = entity_lag(avg_vruntime(cfs_rq), se);
}
```

And in `reweight_eevdf()`, the single critical line changes from:

```c
vlag = (s64)(avruntime - se->vruntime);     // before: unbounded
```

to:

```c
vlag = entity_lag(avruntime, se);            // after: clamped to [-limit, limit]
```

This fix is correct and complete because:

1. **It prevents the s64 overflow:** By clamping vlag to `[-limit, limit]` where `limit = calc_delta_fair(max(2*slice, TICK_NSEC), se)`, the maximum value of `|vlag|` is bounded. For a typical entity with a 4ms slice, `limit ≈ 8ms = 8 × 10⁶ ns`. Even with the largest possible `old_weight` (around 10⁸), the product `vlag * old_weight ≈ 8 × 10⁶ × 10⁸ = 8 × 10¹⁴`, well below `S64_MAX = 9.22 × 10¹⁸`.

2. **It is consistent with the scheduler's steady-state invariants:** The EEVDF scheduler guarantees that in steady state, lag is bounded by `-r_max < lag < max(r_max, q)` where `r_max` is the maximum request size (slice) and `q` is the quantum. The clamping enforces this invariant at the reweight point, which was previously the only path that could violate it.

3. **It preserves the mathematical correctness of the reweight formula:** The formula `v' = V - vl' = V - (vl * w/w')` remains correct; only the input `vl` is now bounded. Since the scheduler already constrains lag at all other points (dequeue, placement), clamping it here simply closes the gap.

## Triggering Conditions

To trigger this bug, the following conditions must be met simultaneously:

**Kernel Configuration:**
- `CONFIG_FAIR_GROUP_SCHED=y` (enabled by default on most distributions) is required because the bug is in `reweight_eevdf()` which is called from `reweight_entity()` which is called from `update_cfs_group()`. Without group scheduling, `update_cfs_group()` is a no-op and `reweight_eevdf()` is never reached through this path.
- `CONFIG_SMP=y` (standard for multi-CPU systems).
- `CONFIG_NO_HZ_FULL=y` or at minimum `CONFIG_NO_HZ_IDLE=y` helps significantly: when tick suppression is enabled, a running task can accumulate a very large virtual time delta in a single `update_curr()` call, creating the large vlag needed to overflow.

**CPU Topology:**
- At least 2 CPUs (one for the driver/idle task, one for the workload). More CPUs increase the likelihood as group weight calculations involve per-CPU load contributions.

**Workload Characteristics:**
- Multiple cgroups (task groups) with runnable CFS tasks, so that `update_cfs_group()` is called on the group scheduling entities at the root CFS runqueue.
- A scenario where one group entity's vruntime diverges significantly from the average vruntime. This happens when: (a) the entity runs for a long time without preemption (e.g., under tick suppression), or (b) many entities are enqueued/dequeued, shifting `avg_vruntime` while one entity remains static.
- A subsequent weight change (via `update_cfs_group()` triggered from `entity_tick()` → `update_load_avg()` → `update_cfs_group()`) while the entity's vruntime is far from the average. The weight change calls `reweight_eevdf()` with the unbounded vlag.

**Specific Overflow Arithmetic:**
- The product `|vlag| × old_weight` must exceed `S64_MAX ≈ 9.22 × 10¹⁸`. With typical group entity weights around 10⁵ to 10⁸ (scaled), vlag needs to be at least 10¹⁰ to 10¹³ nanoseconds (10 to 10000 seconds of virtual time). This requires either tick suppression or a very large accumulation of scheduling imbalance.

**Timing and Probability:**
- The kernel test robot measured a 2.3% reproduction rate with the Trinity syscall fuzzer over 300-second runs on a 2-CPU QEMU VM. Real-world reports came from machines running nightly builds (high fork/exec rates creating frequent `update_cfs_group()` calls) and Android systems with frequent nice value changes.
- The bug is probabilistic: it depends on the exact timing of tick events relative to group weight recalculations. Running more tasks in more cgroups with more dynamic weight changes increases the probability.

## Reproduce Strategy (kSTEP)

The bug can be reproduced with kSTEP by exploiting its ability to configure very large tick intervals (simulating tick suppression), create multiple cgroups with tasks, and trigger weight changes via `kstep_cgroup_set_weight()`.

**Step-by-step reproduction plan:**

1. **Topology setup:** Configure QEMU with at least 2 CPUs (CPU 0 for the driver, CPU 1 for workload tasks). No special topology (SMT, NUMA) is needed.

2. **Cgroup setup:** Create 10 cgroups (task groups), each with a high weight (e.g., 10000). This ensures `CONFIG_FAIR_GROUP_SCHED` is exercised and each group has a corresponding group scheduling entity at the root CFS runqueue of CPU 1.

   ```c
   for (int i = 0; i < 10; i++) {
       kstep_cgroup_create(name[i]);
       kstep_cgroup_set_weight(name[i], 10000);
   }
   ```

3. **Task setup:** Create one CFS task per cgroup and pin all tasks to CPU 1. This creates 10 group scheduling entities competing on CPU 1's root CFS runqueue.

   ```c
   for (int i = 0; i < 10; i++) {
       tasks[i] = kstep_task_create();
       kstep_cgroup_add_task(name[i], tasks[i]->pid);
       kstep_task_pin(tasks[i], 1, 1);
   }
   ```

4. **Tick interval:** Set `tick_interval_ns` to a very large value, such as `10,000,000,000,000` (10 trillion nanoseconds = 10,000 seconds). This simulates the `nohz_full` behavior where the tick is suppressed, causing `update_curr()` to account an enormous virtual time delta in one shot when the tick eventually fires. Each tick, the running entity's vruntime jumps by approximately `tick_interval_ns / 10 ≈ 1000 seconds` of virtual time (divided by ~10 because with 10 entities of equal weight, each entity's virtual time rate is 1/10th of real time... actually it depends on the entity's weight relative to total weight, but the key point is the delta is enormous).

5. **Warm-up phase:** Wake all tasks and run ~20 ticks. This allows the scheduler to cycle through all entities, roughly equalizing vruntimes. After 20 ticks, all group entities have similar vruntimes within ~200 seconds of each other.

   ```c
   for (int i = 0; i < 10; i++)
       kstep_task_wakeup(tasks[i]);
   kstep_tick_repeat(20);
   ```

6. **Create the divergence:** Run one more tick. The currently-running group entity's vruntime advances by ~1000 seconds while the others remain frozen, creating a vlag of approximately -900 billion nanoseconds (-900 seconds).

   ```c
   kstep_tick();
   ```

7. **Trigger the overflow:** Change the weight of the cgroup whose group entity just ran. This triggers `update_cfs_group()` → `reweight_entity()` → `reweight_eevdf()`. Inside `reweight_eevdf()`, the unbounded vlag (~90 × 10⁹) is multiplied by the old weight (~105 × 10⁶), producing a product of ~9.45 × 10¹⁸ which exceeds `S64_MAX`.

   ```c
   int ran = find_running_task_index();
   kstep_cgroup_set_weight(name[ran], 1);  // dramatic weight reduction
   ```

8. **Detect the bug:** After the weight change, read the reweighted entity's vruntime and compare it to a reference entity's vruntime. On the buggy kernel, the vruntime will have drifted by thousands or millions of seconds due to the overflow. On the fixed kernel, the vruntime stays within a reasonable range (bounded by the clamped vlag).

   ```c
   struct sched_entity *ran_se = tasks[ran]->se.parent;  // group entity
   struct sched_entity *ref_se = tasks[ref]->se.parent;  // reference group entity
   s64 drift = (s64)(ran_se->vruntime - ref_se->vruntime);
   s64 abs_drift = drift < 0 ? -drift : drift;
   if (abs_drift > 50000000000000LL)  // > 50,000 seconds
       kstep_fail("vlag overflow detected");
   else
       kstep_pass("vlag properly bounded");
   ```

9. **Pass/fail criteria:**
   - **Buggy kernel (1560d1f6eb6b~1):** The reweighted entity's vruntime drifts by more than 50,000 seconds from the reference entity, indicating the s64 overflow corrupted the vruntime. In the worst case, the system would crash with a NULL pointer dereference when `pick_eevdf()` is called, but we detect the bug before that by examining the vruntime directly.
   - **Fixed kernel (1560d1f6eb6b):** The reweighted entity's vruntime stays within a few hundred seconds of the reference entity (bounded by the tick interval and clamping), and `kstep_pass()` is called.

10. **Callbacks:** No special tick or softirq callbacks are needed. The detection is performed synchronously after the weight change operation. The `on_tick_begin`/`on_tick_end` callbacks could optionally be used to log vruntime values at each tick for debugging purposes.

**Why this works:** The key insight is that `tick_interval_ns` controls how much virtual time is charged per tick via `update_curr()`. By setting it to 10 trillion nanoseconds, each tick simulates a scenario where the running entity accumulates ~1000 seconds of virtual time in one shot — equivalent to what happens on a `nohz_full` system where the tick is suppressed for a long period. This creates the enormous vlag needed to trigger the overflow when the weight is subsequently changed.

**kSTEP API usage:** This approach uses only standard kSTEP APIs: `kstep_cgroup_create/set_weight/add_task`, `kstep_task_create/pin/wakeup`, `kstep_tick/tick_repeat`, and internal state access via `se.parent` (the group entity). The `KSYM_IMPORT` mechanism is not needed since the `sched_entity` fields (vruntime, vlag) are directly accessible through the task structure. An existing driver `kmod/drivers/vlag_overflow.c` already implements this exact strategy.
