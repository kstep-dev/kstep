# Potential Bugs on Linux Master (v7.0-rc2-90-gc107785c7e8d)

Automated audit of 232 known scheduler bugs checked against
Linux master (`c107785c7e8d`). Each entry was analyzed by checking whether
the original bug or a similar pattern still exists in the current code.

## Summary

| Category | Count | Description |
|----------|-------|-------------|
| **A — Regression** | 3 | Fix reverted or regressed |
| **B — Similar pattern** | 7 | Same anti-pattern in other scheduler code path |
| **C — Invariant violated** | 1 | Generic invariant still broken |
| **D — Resolved** | 221 | Bug fully fixed, no similar patterns |
| **Total checked** | 232 | |

## Category A: Regressions / Fix Reverted

| Driver | Commit | Confidence | File | Function | Title |
|--------|--------|------------|------|----------|-------|
| eevdf_reweight_dequeue_avruntime | `afae8002b4fd` | HIGH | fair.c | `reweight_entity` | Refactored reweight_entity() double-counts curr weight in place_entity() inflation |
| eevdf_reweight_vruntime_unadjusted | `eab03c23c2a1` | HIGH | fair.c | `reweight_entity` | place_entity() double-counts curr weight during reweight, breaking lag preservation |
| rt_setprio_push_task_race | `49bef33e4b87` | MEDIUM | rt.c | `push_rt_task` | Proxy scheduling refactor weakened sched_class guard in push_rt_task migration-disabled path |

<details>
<summary>Detailed findings (A)</summary>

### eevdf_reweight_dequeue_avruntime (`afae8002b4fd`)
**Refactored reweight_entity() double-counts curr weight in place_entity() inflation**

Commit 6d71a9c61604 replaced reweight_eevdf() (which used the pre-captured avruntime from the fix) with update_entity_lag() + place_entity(). When se == cfs_rq->curr, place_entity()'s lag inflation formula counts curr's weight twice: load = sum_weight + w_curr (via the curr check), then inflated_lag = vlag * (load + w_se) / load where w_se == w_curr, yielding (W+2w)/(W+w) instead of the correct (W+w)/W. This produces ~11% weighted-lag error for typical weight ratios (e.g., W=1024, w=512), violating the lag preservation invariant. The original fix's approach (passing pre-dequeue V directly) was correct for both curr and non-curr paths.

### eevdf_reweight_vruntime_unadjusted (`eab03c23c2a1`)
**place_entity() double-counts curr weight during reweight, breaking lag preservation**

Commit 6d71a9c61604 replaced reweight_eevdf() with place_entity() for vruntime recalculation during reweight. When se==cfs_rq->curr, place_entity()'s lag inflation adds curr->load.weight to 'load' AND se->load.weight (same entity), computing inflation factor (W+2w')/(W+w') instead of the correct (W+w')/W. Additionally, avg_vruntime() inside place_entity() uses the NEW weight with the OLD vruntime, yielding V_now!=V_old. Numerical verification: weight increase 335→1024 gives 75% lag error; weight decrease 1024→335 gives 5.3% error. The original reweight_eevdf() correctly used V_old directly (v'=V-(V-v)*w/w') without inflation, handling both curr and non-curr cases.

### rt_setprio_push_task_race (`49bef33e4b87`)
**Proxy scheduling refactor weakened sched_class guard in push_rt_task migration-disabled path**

The fix (49bef33e4b87) added a guard checking rq->curr->sched_class before calling find_lowest_rq(rq->curr). Commit af0c8b2bf67b (proxy scheduling) changed the guard to check rq->donor->sched_class but left find_lowest_rq(rq->curr) unchanged. With CONFIG_SCHED_PROXY_EXEC=y (EXPERT), rq->donor (RT) can differ from rq->curr (CFS proxy), so the guard passes while find_lowest_rq receives a non-RT task, causing UB in convert_prio() for CFS priorities (101-139) that fall through its switch statement. Additionally, get_push_task() returns rq->donor not rq->curr, creating a mismatch with the CPU found for rq->curr.

</details>

## Category B: Similar Patterns in Scheduler

| Driver | Commit | Confidence | File | Function | Title |
|--------|--------|------------|------|----------|-------|
| core_preempt_dynamic_return_value | `9ed20bafc858` | HIGH | core.c | `setup_resched_latency_warn_ms` | setup_resched_latency_warn_ms() returns 1 on parse failure, same inverted __setup return pattern |
| deadline_hrtick_enabled_wrong_check | `d16b7eb6f523` | MEDIUM | fair.c | `task_tick_fair` | task_tick_fair() calls hrtick_start_fair() without hrtick_enabled_fair() check |
| fair_wake_affine_delayed_dequeue | `aa3ee4f0b754` | HIGH | fair.c | `select_idle_sibling` | select_idle_sibling per-cpu kthread stacking uses raw nr_running without subtracting delayed tasks |
| lb_avg_load_condition | `6c8116c914b6` | MEDIUM | fair.c | `update_sg_lb_stats` | LB path update_sg_lb_stats omits avg_load for group_fully_busy same as original wakeup bug |
| pelt_rt_policy_change_spike | `fecfcbc288e9` | MEDIUM | deadline.c | `switched_to_dl` | switched_to_dl() uses rq->donor instead of task_current() for PELT sync under CONFIG_SCHED_PROXY_EXEC |
| topology_numa_cpuless_node_crash | `617f2c38cb5c` | MEDIUM | topology.c | `sched_numa_hop_mask` | sched_numa_hop_mask() lacks CPU-less node validation same as original bug |
| uclamp_fork_reset_rt_boost | `eaf5a92ebde5` | MEDIUM | syscalls.c | `__setscheduler_uclamp` | __setscheduler_uclamp uses rt_task(p) which includes PI-boosted tasks, can set wrong uclamp.min for SCHED_NORMAL |

<details>
<summary>Detailed findings (B)</summary>

### core_preempt_dynamic_return_value (`9ed20bafc858`)
**setup_resched_latency_warn_ms() returns 1 on parse failure, same inverted __setup return pattern**

The fix corrected setup_preempt_mode() to return 0 on failure and 1 on success per __setup convention, but setup_resched_latency_warn_ms() (core.c:5525) has the identical anti-pattern: when kstrtol() fails to parse the string, it prints pr_warn but returns 1 instead of 0, telling __setup the parameter was handled and suppressing the "Unknown kernel command line parameters" warning. The same pattern also exists in setup_rt_group_sched() (core.c:9997) and setup_relax_domain_level() (topology.c:1507), both returning 1 on parse failure.

### deadline_hrtick_enabled_wrong_check (`d16b7eb6f523`)
**task_tick_fair() calls hrtick_start_fair() without hrtick_enabled_fair() check**

Commit 95a0155224a6 ("sched/fair: Limit hrtick work", 2025-09-01) added a new hrtick_start_fair() call in task_tick_fair() at line 13444 without the hrtick_enabled_fair(rq) guard that all other CFS hrtick call sites use (__set_next_task_fair and hrtick_update both check it). This is the same anti-pattern as the original DL bug: a start_hrtick function called without the class-specific feature check. If sched_feat(HRTICK) is enabled at runtime and then disabled, the hrtick timer will keep re-arming through this unguarded path since the queued=1 hrtick callback re-enters task_tick_fair which re-arms without checking the feature flag. Under default settings (HRTICK=false) the path is unreachable since hrtick is never initially armed, limiting practical impact to runtime sched_feat toggling scenarios.

### fair_wake_affine_delayed_dequeue (`aa3ee4f0b754`)
**select_idle_sibling per-cpu kthread stacking uses raw nr_running without subtracting delayed tasks**

The per-cpu kthread stacking optimization in select_idle_sibling() (line ~7892) checks `this_rq()->nr_running <= 1` without subtracting delayed-dequeued tasks, identical to the original wake_affine_idle bug. When a per-cpu kthread (e.g., IO completion kworker) is the only truly running task but delayed tasks inflate nr_running, the check fails and the wakee is not stacked on the kthread's CPU. The helper cfs_h_nr_delayed() exists and is used in wake_affine_idle but was not applied here. The kthread stacking path is the primary placement path when the CPU is not idle (kthread is running), so bypassing it forces fallback to select_idle_cpu(), breaking IO completion cache affinity.

### lb_avg_load_condition (`6c8116c914b6`)
**LB path update_sg_lb_stats omits avg_load for group_fully_busy same as original wakeup bug**

The fix correctly resolved the wakeup path in update_sg_wakeup_stats(), which now computes avg_load for both group_fully_busy and group_overloaded. However, the load balancer path in update_sg_lb_stats() (line ~10548) only computes avg_load for group_overloaded, while update_sd_pick_busiest() uses avg_load for group_fully_busy comparisons (lines 10634-10650). An explicit XXX comment at line 10642 acknowledges avg_load is always 0 for fully_busy, making busiest-group selection arbitrary rather than load-aware. This is the same anti-pattern as the original bug but in the LB path; however, it has been documented since the original rework (commit 0b0695f2b34a) and may be an intentional design tradeoff.

### pelt_rt_policy_change_spike (`fecfcbc288e9`)
**switched_to_dl() uses rq->donor instead of task_current() for PELT sync under CONFIG_SCHED_PROXY_EXEC**

The RT fix (fecfcbc288e9) correctly uses task_current(rq, p) (checks rq->curr) in switched_to_rt() to sync PELT avg_rt on policy change. The companion DL fix (d7d607096ae6) originally used rq->curr != p in switched_to_dl(), but commit af0c8b2bf67b (proxy scheduling refactor) changed this to rq->donor != p. Under CONFIG_SCHED_PROXY_EXEC=y, when the executing task (rq->curr) differs from the scheduling context (rq->donor) during proxy execution and its policy is changed to SCHED_DEADLINE, the PELT avg_dl.last_update_time is not synchronized, reproducing the same stale-timestamp spike pattern as the original RT bug. This only affects the experimental EXPERT-gated CONFIG_SCHED_PROXY_EXEC feature.

### topology_numa_cpuless_node_crash (`617f2c38cb5c`)
**sched_numa_hop_mask() lacks CPU-less node validation same as original bug**

The fix added numa_nearest_node() validation in sched_numa_find_nth_cpu() to handle CPU-less NUMA nodes, but sched_numa_hop_mask() in the same file (line 2346) has the identical anti-pattern: it directly returns masks[hops][node] without checking if node has CPUs. For CPU-less nodes, this returns NULL instead of ERR_PTR, violating the function's documented API contract. The for_each_numa_hop_mask macro mitigates this with IS_ERR_OR_NULL, but any direct caller using only IS_ERR() would get a NULL dereference.

### uclamp_fork_reset_rt_boost (`eaf5a92ebde5`)
**__setscheduler_uclamp uses rt_task(p) which includes PI-boosted tasks, can set wrong uclamp.min for SCHED_NORMAL**

In kernel/sched/syscalls.c, __setscheduler_uclamp() checks rt_task(p) (line ~365) to decide whether to set uclamp.min to the RT default (1024). However, rt_task() checks p->prio which reflects PI boosting, not p->policy. When sched_setattr() is called on a SCHED_NORMAL task that is PI-boosted to RT priority (e.g., changing its nice value while it holds an rt_mutex contended by an RT task), rt_task(p) returns true and uclamp_req[UCLAMP_MIN] is set to sysctl_sched_uclamp_util_min_rt_default (1024). When the PI boost is later removed via rt_mutex_setprio(), uclamp is never corrected, leaving a SCHED_NORMAL task permanently with max-boost uclamp.min. The check should use task_has_rt_policy(p) instead, matching the same anti-pattern as the original fork bug.

</details>

## Category C: Invariant Still Violated

| Driver | Commit | Confidence | File | Function | Title |
|--------|--------|------------|------|----------|-------|
| bandwidth_throttle_runnable_avg | `6212437f0f60` | MEDIUM | fair.c | `set_delayed` | set_delayed() modifies h_nr_runnable without updating ancestor runnable_weight |

<details>
<summary>Detailed findings (C)</summary>

### bandwidth_throttle_runnable_avg (`6212437f0f60`)
**set_delayed() modifies h_nr_runnable without updating ancestor runnable_weight**

The fix commit is on master and the original throttle/unthrottle code was completely reworked (task-based throttle model). However, the invariant (se->runnable_weight == se->my_q->h_nr_runnable for on_rq group entities) is still violable via set_delayed()/clear_delayed(). When a task entity is delay-dequeued, set_delayed() decrements cfs_rq->h_nr_runnable at every ancestor level but never calls se_update_runnable() for ancestor group entities, leaving their runnable_weight stale. Since dequeue_entities() returns -1 for delayed tasks (skipping the second for_each_sched_entity loop that calls se_update_runnable), and entity_tick() also omits se_update_runnable(), the stale values feed into PELT via __update_load_avg_se()/se_runnable() across multiple ticks until the next enqueue/dequeue event in the hierarchy. Commit 3429dd57f0de acknowledged this PELT inaccuracy as self-correcting but did not add the missing se_update_runnable() calls.

</details>

