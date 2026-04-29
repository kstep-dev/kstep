# Fair: SCHED_IDLE Tasks Skip reweight_task() on Weight Change

**Commit:** `d329605287020c3d1c3b0dadc63d8208e7251382`
**Affected files:** kernel/sched/core.c, kernel/sched/fair.c, kernel/sched/sched.h
**Fixed in:** v6.11-rc1
**Buggy since:** v4.15-rc1 (introduced by commit `9059393e4ec1` "sched/fair: Use reweight_entity() for set_user_nice()")

## Bug Description

When the Linux kernel scheduler changes a task's weight (due to nice value changes via `set_user_nice()` or scheduling policy changes via `sched_setscheduler()`), the function `set_load_weight()` in `kernel/sched/core.c` is called with the `update_load` parameter set to `true`. For CFS (fair-class) tasks, this function delegates to `reweight_task()` in `fair.c`, which performs the non-trivial work of properly updating the CFS runqueue's load accounting, the entity's vlag (virtual lag), the entity's position in the EEVDF/CFS RB tree, and the PELT load averages.

However, `set_load_weight()` contains an early-return path for `SCHED_IDLE` tasks. The function checks `task_has_idle_policy(p)` at its entry, and if the task uses the SCHED_IDLE policy, it directly writes the idle weight (`WEIGHT_IDLEPRIO = 3`, `WMULT_IDLEPRIO`) to `p->se.load` and returns immediately — without ever calling `reweight_task()`. This is incorrect because SCHED_IDLE tasks are fundamentally just CFS tasks with a very low weight; they are scheduled by the same fair scheduling class (`fair_sched_class`) and use the same CFS runqueue data structures.

The consequence is that when a CFS task transitions to SCHED_IDLE policy (e.g., via `sched_setscheduler(pid, SCHED_IDLE, ...)`), or when a running SCHED_IDLE task already on a CFS runqueue has its parameters changed, the critical CFS accounting updates in `reweight_entity()` are skipped entirely. This leads to inconsistent CFS runqueue state: stale load sums, unscaled vlag values, incorrect entity positions in the RB tree, and corrupted PELT load averages.

The bug was introduced in commit `9059393e4ec1` (v4.15) when `set_load_weight()` was refactored to call `reweight_task()` for fair-class tasks. The original `set_load_weight()` function was a simple direct assignment and did not have the distinction between `update_load` true/false paths. The SCHED_IDLE early-return was added earlier (when SCHED_IDLE was introduced) for the simple weight-set case, but was never updated when the `reweight_task()` delegation was added. As Peter Zijlstra noted in the mailing list discussion: "IDLE really is FAIR but with a really small weight."

## Root Cause

The root cause is a logic error in `set_load_weight()` in `kernel/sched/core.c`. The buggy code has this structure:

```c
void set_load_weight(struct task_struct *p, bool update_load)
{
    int prio = p->static_prio - MAX_RT_PRIO;
    struct load_weight *load = &p->se.load;

    /* SCHED_IDLE tasks get minimal weight: */
    if (task_has_idle_policy(p)) {
        load->weight = scale_load(WEIGHT_IDLEPRIO);
        load->inv_weight = WMULT_IDLEPRIO;
        return;    // <--- BUG: skips reweight_task() even when update_load is true
    }

    /* SCHED_OTHER tasks have to update their load when changing their weight */
    if (update_load && p->sched_class == &fair_sched_class) {
        reweight_task(p, prio);
    } else {
        load->weight = scale_load(sched_prio_to_weight[prio]);
        load->inv_weight = sched_prio_to_wmult[prio];
    }
}
```

The `task_has_idle_policy()` check and early return is placed **before** the `update_load` check. When `update_load` is `true` and the task is SCHED_IDLE, the function returns after setting the weight directly on `p->se.load` without going through `reweight_task()`.

The `reweight_task()` function (in `fair.c`) calls `reweight_entity()`, which performs several critical operations:

1. **Commits outstanding execution time**: Calls `update_curr(cfs_rq)` to account for any time the entity has been running since the last update.

2. **Dequeues the entity from the CFS RB tree**: If the entity is on the runqueue and not the current task, it is removed via `__dequeue_entity()` to allow repositioning.

3. **Updates CFS runqueue load**: Subtracts the old weight from `cfs_rq->load` via `update_load_sub()` and later adds the new weight via `update_load_add()`.

4. **Scales vlag for the weight change**: For on-rq entities, calls `reweight_eevdf()` which adjusts the entity's virtual deadline, vruntime, and vlag according to the EEVDF algorithm. For off-rq entities, scales vlag proportionally: `se->vlag = div_s64(se->vlag * se->load.weight, weight)`.

5. **Updates PELT load averages**: Recalculates `se->avg.load_avg` based on the new weight: `se->avg.load_avg = div_u64(se_weight(se) * se->avg.load_sum, divider)`. Also dequeues and re-enqueues the load average from the CFS runqueue.

6. **Re-enqueues and repositions**: Re-inserts the entity into the CFS RB tree at its new position (reflecting the updated vruntime/deadline) and updates `min_vruntime`.

When `set_load_weight()` bypasses `reweight_task()`, none of these operations occur. The weight is changed directly on `se->load`, but:
- The CFS runqueue's `cfs_rq->load` still reflects the old weight
- The entity remains at its old position in the RB tree
- The entity's vlag is not scaled for the weight change
- The PELT `load_avg` is not recalculated

The primary triggering scenario is transitioning a running CFS task TO the SCHED_IDLE policy. When `__setscheduler_params()` is called, it first sets `p->policy = SCHED_IDLE`, then calls `set_load_weight(p, true)`. At this point `task_has_idle_policy(p)` is true, triggering the early return. The task's weight drops from its previous value (e.g., 1024 for nice 0) to 3 (`WEIGHT_IDLEPRIO`), but the CFS accounting is not updated to reflect this ~341x weight reduction.

## Consequence

The most direct consequence is inconsistent CFS runqueue load accounting. When a task transitions to SCHED_IDLE without proper reweighting, the `cfs_rq->load.weight` sum no longer matches the sum of its constituent entities' weights. This affects CFS's proportional fairness calculations, as the runqueue load weight is used in computing virtual runtime advancement and in load balancing decisions.

The entity's vlag (virtual lag, used in EEVDF scheduling) is not scaled for the weight change. Since `vlag = V - v_i` and `lag_i = w_i * (V - v_i)`, failing to scale vlag when the weight changes from a large value (e.g., 1024) to a tiny value (3) means the entity's lag is effectively divided by ~341x implicitly, which can cause the entity to appear far more or less "behind" than it should be in the EEVDF virtual time calculation. This was the specific concern raised by Xuewen Yan on the LKML, who observed vlag going out of bounds on Android systems where task nice values change very frequently. The entity also remains at its old position in the CFS RB tree, meaning it will be selected for scheduling based on a stale vruntime/deadline rather than one adjusted for its new weight.

The PELT (Per-Entity Load Tracking) load average (`se->avg.load_avg`) becomes stale and inconsistent with the actual weight. Since `load_avg` is computed as `weight * load_sum / divider`, changing the weight without recalculating `load_avg` means the entity contributes an incorrect amount to the runqueue's aggregate load. This can cascade to incorrect load balancing decisions, incorrect CPU frequency scaling through schedutil, and incorrect energy-aware scheduling decisions. While this bug is unlikely to cause a kernel crash or data corruption, it results in scheduling unfairness, incorrect load distribution across CPUs, and potentially degraded performance or power efficiency, particularly in systems that frequently change task policies between SCHED_NORMAL and SCHED_IDLE (such as Android systems managing foreground/background task priorities).

## Fix Summary

The fix restructures `set_load_weight()` to unconditionally compute the target `load_weight` (either SCHED_IDLE weight or priority-based weight) into a local `struct load_weight lw` variable, and then decide whether to apply it via `reweight_task()` (when `update_load` is true and the task is a fair-class task) or via direct assignment (`p->se.load = lw`). The key change is that the SCHED_IDLE early-return path is eliminated — SCHED_IDLE tasks now go through the same `reweight_task()` path as any other fair-class task when `update_load` is set.

The fixed code looks like:

```c
void set_load_weight(struct task_struct *p, bool update_load)
{
    int prio = p->static_prio - MAX_RT_PRIO;
    struct load_weight lw;

    if (task_has_idle_policy(p)) {
        lw.weight = scale_load(WEIGHT_IDLEPRIO);
        lw.inv_weight = WMULT_IDLEPRIO;
    } else {
        lw.weight = scale_load(sched_prio_to_weight[prio]);
        lw.inv_weight = sched_prio_to_wmult[prio];
    }

    if (update_load && p->sched_class == &fair_sched_class)
        reweight_task(p, &lw);
    else
        p->se.load = lw;
}
```

Additionally, `reweight_task()` is changed from taking an `int prio` parameter (which it used to convert to a weight internally) to taking a `const struct load_weight *lw` directly. This is necessary because SCHED_IDLE weight (`WEIGHT_IDLEPRIO = 3`) cannot be expressed as a priority index into `sched_prio_to_weight[]` — it is a special hard-coded value. By passing the computed `load_weight` struct directly, `reweight_task()` can correctly handle both normal priority-based weights and the special SCHED_IDLE weight. The declaration in `sched.h` is updated accordingly from `extern void reweight_task(struct task_struct *p, int prio)` to `extern void reweight_task(struct task_struct *p, const struct load_weight *lw)`.

## Triggering Conditions

The bug is triggered whenever `set_load_weight()` is called with `update_load=true` for a task that has the SCHED_IDLE policy and belongs to the fair scheduling class. This occurs in two primary call sites:

1. **`__setscheduler_params()`** (called from `sched_setscheduler()` and related functions): When a task's scheduling policy is changed TO SCHED_IDLE from any other policy, `p->policy` is set to `SCHED_IDLE` first, then `set_load_weight(p, true)` is called. At this point `task_has_idle_policy(p)` returns true and the early return is taken, skipping `reweight_task()`.

2. **`set_user_nice()`**: If a task is already running as SCHED_IDLE and its nice value is changed (e.g., via `sys_setpriority()` or `renice`), `set_load_weight(p, true)` is called. While the actual weight for SCHED_IDLE is always `WEIGHT_IDLEPRIO` regardless of nice value (so the weight doesn't change), the `reweight_task()` call is still skipped. In practice this particular sub-case is less impactful since the weight doesn't change, but it still means the load accounting path is not exercised.

The most impactful scenario requires:
- At least one CFS task that is **currently on a CFS runqueue** (either running or enqueued)
- The task's scheduling policy is changed to `SCHED_IDLE` while it is on the runqueue
- No special kernel configuration beyond standard CFS support is needed
- No specific topology or CPU count requirements (any number of CPUs will do)
- No race condition or timing sensitivity — the bug is deterministic: every transition to SCHED_IDLE will skip reweight_task()

The bug is 100% reproducible whenever the above conditions are met. No special timing, concurrency, or probability is involved. On Android systems where task scheduling policies are changed frequently (e.g., moving tasks between foreground/background), this bug would be triggered routinely.

## Reproduce Strategy (kSTEP)

To reproduce this bug in kSTEP, we need to:

1. **Add a `kstep_task_idle()` function** (minor kSTEP extension): kSTEP currently has `kstep_task_cfs()` and `kstep_task_fifo()` to change a task's scheduling policy, but no function to set SCHED_IDLE. This is a trivial addition to `kmod/task.c`:
   ```c
   void kstep_task_idle(struct task_struct *p) {
       struct sched_attr attr = {
           .sched_policy = SCHED_IDLE,
           .sched_nice = 0,
       };
       sched_setattr_nocheck(p, &attr);
       kstep_sleep();
   }
   ```
   Also add the prototype to `kmod/driver.h`. This is a one-line change to the existing pattern.

2. **Create a CFS task with default weight (nice 0, weight 1024)**:
   ```c
   struct task_struct *p = kstep_task_create();
   kstep_task_pin(p, 1, 1);  // Pin to CPU 1 (not CPU 0)
   kstep_task_wakeup(p);
   ```

3. **Let the task build up scheduling state**: Run several ticks so the task accumulates vruntime, vlag, and PELT load averages:
   ```c
   kstep_tick_repeat(100);  // Build up scheduling state
   ```

4. **Record pre-transition CFS accounting state**: Before changing the policy, read and record the current CFS runqueue state using kSTEP's internal access:
   ```c
   struct rq *rq = cpu_rq(1);
   struct cfs_rq *cfs = &rq->cfs;
   struct sched_entity *se = &p->se;
   
   unsigned long old_se_weight = se->load.weight;
   unsigned long old_cfs_load = cfs->load.weight;
   s64 old_vlag = se->vlag;
   unsigned long old_load_avg = se->avg.load_avg;
   ```

5. **Transition the task to SCHED_IDLE**: This triggers the buggy path:
   ```c
   kstep_task_idle(p);
   ```

6. **Verify the bug by checking CFS accounting**: After the policy change, verify what happened:
   ```c
   unsigned long new_se_weight = se->load.weight;
   unsigned long new_cfs_load = cfs->load.weight;
   s64 new_vlag = se->vlag;
   unsigned long new_load_avg = se->avg.load_avg;
   
   // Check 1: The entity weight should have changed to WEIGHT_IDLEPRIO (3)
   // This will be true on both buggy and fixed kernels
   
   // Check 2: CFS runqueue load should reflect the new weight
   // On buggy kernel: cfs->load.weight is STALE (still includes old weight, missing subtraction/addition)
   // On fixed kernel: cfs->load.weight is correctly updated
   unsigned long expected_cfs_load = old_cfs_load - old_se_weight + scale_load(3);
   if (new_cfs_load != expected_cfs_load) {
       kstep_fail("cfs_rq load inconsistent: expected %lu, got %lu",
                  expected_cfs_load, new_cfs_load);
   }
   
   // Check 3: vlag should be scaled proportionally
   // On buggy kernel: vlag is UNCHANGED (still at old value)
   // On fixed kernel: vlag is scaled by old_weight / new_weight
   if (old_vlag != 0 && new_vlag == old_vlag) {
       kstep_fail("vlag not scaled: still %lld after weight change %lu -> %lu",
                  new_vlag, old_se_weight, new_se_weight);
   }
   ```

7. **Pass/fail criteria**:
   - **Buggy kernel**: After `kstep_task_idle(p)`, the `cfs_rq->load.weight` will NOT be updated (it still reflects the old entity weight), and `se->vlag` will NOT be scaled. The entity's PELT `load_avg` will still reflect the old weight. `kstep_fail()` is triggered.
   - **Fixed kernel**: After `kstep_task_idle(p)`, `reweight_task()` is called, which invokes `reweight_entity()`. This properly: (a) subtracts old weight from `cfs_rq->load`, (b) calls `reweight_eevdf()` or scales vlag, (c) recalculates `load_avg`, (d) re-adds new weight to `cfs_rq->load`, (e) repositions entity in RB tree. `kstep_pass()` is triggered.

8. **Additional test variant** — To make the test more robust, also test the reverse transition (SCHED_IDLE → SCHED_NORMAL) which should work correctly on both buggy and fixed kernels, confirming the asymmetry:
   ```c
   // Start as SCHED_IDLE, transition to normal CFS
   struct task_struct *p2 = kstep_task_create();
   kstep_task_pin(p2, 1, 1);
   kstep_task_idle(p2);
   kstep_task_wakeup(p2);
   kstep_tick_repeat(100);
   // Record state, then:
   kstep_task_cfs(p2);
   // Verify reweight was properly called (should pass on both kernels)
   ```

9. **Multi-task variant for more visible impact**: Create multiple tasks on the same CPU to amplify the load accounting discrepancy:
   ```c
   struct task_struct *tasks[4];
   for (int i = 0; i < 4; i++) {
       tasks[i] = kstep_task_create();
       kstep_task_pin(tasks[i], 1, 1);
       kstep_task_wakeup(tasks[i]);
   }
   kstep_tick_repeat(100);
   // Transition all 4 to SCHED_IDLE
   for (int i = 0; i < 4; i++)
       kstep_task_idle(tasks[i]);
   // The cfs_rq load discrepancy is now 4x larger
   ```

10. **QEMU configuration**: 2 CPUs minimum (CPU 0 for the driver, CPU 1 for the test tasks). No special memory or topology requirements.

11. **Kernel version guard**: Use `#if LINUX_VERSION_CODE` to target the buggy range. The bug exists from v4.15 through v6.10. The fix was merged into v6.11-rc1. Since kSTEP supports v5.15+, the guard should be:
    ```c
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) && \
        LINUX_VERSION_CODE < KERNEL_VERSION(6, 11, 0)
    ```

12. **kSTEP extension needed**: The only framework change required is adding `kstep_task_idle()` to `kmod/task.c` and its prototype to `kmod/driver.h`. This follows the exact same pattern as the existing `kstep_task_cfs()` and `kstep_task_fifo()` functions and is a ~10-line addition. No other kSTEP changes are needed — all the observation can be done via `KSYM_IMPORT` and direct access to `cpu_rq()`, `cfs_rq`, and `sched_entity` fields through kSTEP's `internal.h`.
