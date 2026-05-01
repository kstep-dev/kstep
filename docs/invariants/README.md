# Scheduler Invariants

Generic scheduler invariants derived from bug analysis. Each invariant is a
property that should always hold in a correct scheduler. Only bugs that yield
a clear, reusable invariant are listed — ad-hoc bugs are omitted.

**Total: 96 invariants across 13 categories.**

## Table of Contents

1. [Accounting Consistency](#accounting-consistency) (5)
2. [PELT / Load Tracking](#pelt--load-tracking) (12)
3. [CFS Bandwidth](#cfs-bandwidth) (5)
4. [Leaf List / cfs_rq Membership](#leaf-list--cfs_rq-membership) (7)
5. [Deadline / DL Server](#deadline--dl-server) (11)
6. [Core Scheduling](#core-scheduling) (4)
7. [Uclamp](#uclamp) (13)
8. [Task State / Lifecycle](#task-state--lifecycle) (8)
9. [Weight / Reweight](#weight--reweight) (6)
10. [Vruntime / Eligibility](#vruntime--eligibility) (5)
11. [EEVDF Pick & Scheduling](#eevdf-pick--scheduling) (10)
12. [Load Balancing](#load-balancing) (7)
13. [RT Scheduling](#rt-scheduling) (3)

---

## Accounting Consistency

Invariants ensuring nr_running, h_nr_running, h_nr_delayed, and nr_iowait counters accurately reflect the actual set of queued tasks.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| bandwidth_enqueue_dequeue_reorder | `5ab297bab984` | h_nr_running Hierarchical Consistency | `h_nr_running` must equal directly enqueued task entities plus sum of child group `h_nr_running` | enqueue_task_fair, dequeue_task_fair |
| core_nr_iowait_ordering | `ec618b84f6e1` | Non-negative runqueue iowait counter | `rq->nr_iowait` must always be non-negative (counts tasks in I/O wait state) | scheduler_tick, ttwu_do_activate |
| deadline_dlserver_nr_running_doublecount | `52d15521eb75` | enqueue/dequeue nr_running delta must be exactly 1 | A single enqueue/dequeue_task() call must change rq->nr_running by exactly 1 | enqueue_task, dequeue_task |
| eevdf_h_nr_delayed_group_entity | `3429dd57f0de` | h_nr_delayed Must Equal Count of Delayed Task Entities in Hierarchy | For any cfs_rq, `h_nr_delayed` must equal the count of task entities with `sched_delayed == 1` | dequeue_task_fair, enqueue_task_fair |
| rt_dequeue_tick_reenable | `5c66d1b9b30f` | nr_running Consistency with Per-Class Task Counts | At any call to `sched_update_tick_dependency()`, `rq->nr_running` must equal sum of per-class counts | sched_update_tick_dependency |

## PELT / Load Tracking

Invariants on Per-Entity Load Tracking signals: util_avg, load_avg, load_sum, runnable_avg bounds, clock_pelt synchronization, and propagation consistency.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| deadline_dl_util_policy_change | `d7d607096ae6` | Per-RQ PELT last_update_time Synchronized After Class Transition | After switched_to_*() for a running task, new class's per-rq PELT must sync last_update_time | switched_to_dl, switched_to_rt |
| pelt_attach_load_sum_zero | `40f5aa4c5eae` | PELT avg/sum Consistency | For any `cfs_rq` or `sched_entity`, if a PELT `_avg` field is non-zero then `_sum` must also be non-zero | attach_entity_load_avg, cfs_rq_is_decayed, dequeue_load_avg, enqueue_load_avg |
| pelt_cfs_rq_h_nr_delayed | `76f2f783294d` | CFS runqueue effective runnable count consistency | Effective runnable count (h_nr_running - h_nr_delayed) must equal actual non-delayed entities | __update_load_avg_cfs_rq, scheduler_tick, se_update_runnable |
| pelt_dequeue_load_sum_desync | `ceb6ba45dc80` | PELT sum-avg zero consistency | If a PELT `*_sum` field is zero then the corresponding `*_avg` field must also be zero | dequeue_load_avg, __update_blocked_fair, update_cfs_rq_load_avg |
| pelt_hw_pressure_clock_mismatch | `84d265281d6c` | PELT Clock Domain Consistency for per-rq sched_avg | Each per-rq sched_avg's last_update_time must stay in its designated clock domain | sched_tick, __update_blocked_others |
| pelt_lost_idle_time_slow_path | `17e3e88ed0b6` | PELT clock_pelt must be synced when rq goes idle at max utilization | When rq goes idle with util_sum >= max threshold, clock_pelt must equal rq_clock_task(rq) | pick_next_task (idle selection), post-idle transition |
| pelt_propagate_load_sum_desync | `7c7ad626d9a0` | PELT avg/sum Consistency | For any cfs_rq, if a PELT `*_avg` field is nonzero then `*_sum` must also be nonzero, and vice versa | update_tg_cfs_load, update_blocked_averages, update_load_avg, dequeue_load_avg |
| pelt_rt_policy_change_spike | `fecfcbc288e9` | PELT per-rq last_update_time must be current at scheduling class transitions | Per-rq PELT `last_update_time` must be synced to `rq_clock_pelt(rq)` before accumulating under new class | switched_to_rt(), switched_to_dl() |
| pelt_runnable_avg_init_overload | `e21cf43406a1` | New Entity Runnable-Util Consistency | When a new entity is first initialized, its `runnable_avg` must equal its `util_avg` | post_init_entity_util_avg, attach_entity_load_avg |
| pelt_tg_load_avg_stale_propagate | `02da26ad5ed6` | Decayed cfs_rq Must Have Zero tg_load_avg_contrib | When a cfs_rq is fully decayed, its `tg_load_avg_contrib` must be zero | list_del_leaf_cfs_rq, on_sched_softirq_end |
| pelt_throttled_clock_mismatch | `64eaf50731ac` | cfs_rq_clock_pelt Bounded by rq_clock_pelt | For any cfs_rq, `cfs_rq_clock_pelt(cfs_rq)` must never exceed `rq_clock_pelt(rq_of(cfs_rq))` | update_load_avg, tg_unthrottle_up |
| pelt_util_sum_sync_loss | `98b0d890220d` | PELT _sum >= _avg * PELT_MIN_DIVIDER Consistency | For any PELT signal, `_sum` must always be >= `_avg * PELT_MIN_DIVIDER` | update_cfs_rq_load_avg, update_tg_cfs_util, __update_load_avg_se |

## CFS Bandwidth

Invariants on CFS bandwidth control: throttle/unthrottle correctness, runtime distribution, quota hierarchy, and throttled entity state.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| bandwidth_hierarchical_quota_cgroupv2 | `c98c18270be1` | Hierarchical Quota Monotonicity | Child's `hierarchical_quota` must never be less restrictive than parent's (`child_hq ≤ parent_hq`) | tg_cfs_schedulable_down, init_cfs_bandwidth |
| bandwidth_pelt_clock_sync_throttle | `0e4a169d1a2b` | PELT Clock Frozen Consistency for Throttled Empty cfs_rqs | If a cfs_rq is throttled (throttle_count > 0) and empty (nr_running == 0), PELT clock must be frozen | sync_throttle, propagate_entity_cfs_rq, enqueue_entity/dequeue_entity |
| bandwidth_throttle_block_delayed | `e67e3e738f08` | Delayed Dequeue Must Block Task | After `dequeue_task()` with `DEQUEUE_DELAYED`, the task must be blocked (`p->on_rq == 0`) | dequeue_task_fair |
| bandwidth_throttle_runnable_avg | `6212437f0f60` | Group Entity runnable_weight Consistency | For any group entity `se` on a runqueue, `se->runnable_weight` must equal `se->my_q->h_nr_running` | throttle_cfs_rq, unthrottle_cfs_rq, update_curr |
| bandwidth_unthrottle_delayed_entity | `9b5ce1a37e90` | Delayed Group Entity Must Not Have Runnable Children | If a group entity has `sched_delayed == true`, its group `cfs_rq` must have `h_nr_running == 0` | unthrottle_cfs_rq, enqueue_entity, scheduler_tick/update_curr |

## Leaf List / cfs_rq Membership

Invariants ensuring leaf_cfs_rq_list completeness: every active cfs_rq and its ancestors must be on the list, and entries must belong to live task groups.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| bandwidth_leaf_list_throttle_fixup | `b34cb07dde7c` | Leaf CFS RQ List Ancestor Completeness | For every non-root cfs_rq on rq->leaf_cfs_rq_list, its parent must also be on the list | enqueue_task_fair, unthrottle_cfs_rq |
| bandwidth_unthrottle_decayed_cfsrq | `a7b359fc6a37` | Non-decayed unthrottled cfs_rq must be on the leaf list | If a cfs_rq is not throttled and has non-decayed PELT state or running entities, it must be on leaf list | unthrottle_cfs_rq, update_blocked_averages |
| bandwidth_unthrottle_decayed_parent | `fdaba61ef8a2` | Leaf CFS RQ List Ancestor Completeness | If a cfs_rq is on leaf_cfs_rq_list, every ancestor cfs_rq must also be on the list | unthrottle_cfs_rq, __update_blocked_fair |
| bandwidth_unthrottle_leaf_ancestor | `2630cde26711` | Leaf List Ancestor Completeness | If a `cfs_rq` is on the leaf list, every ancestor `cfs_rq` up to root must also be on it | unthrottle_cfs_rq, enqueue_task_fair, sched_move_task |
| bandwidth_unthrottle_leaf_fixup | `39f23ce07b93` | Active cfs_rq Must Be On Leaf List | Every non-throttled cfs_rq with nr_running > 0 must be on rq's leaf_cfs_rq_list (on_list == 1) | unthrottle_cfs_rq, enqueue_task_fair |
| cgroup_dead_tg_cfsrq_uaf | `b027789e5e50` | Leaf-list cfs_rq must belong to a live task group | Every cfs_rq on a per-rq leaf list must belong to a task group still reachable from `task_groups` | update_blocked_averages, list_add_leaf_cfs_rq |
| pelt_missing_leaf_load_decay | `0258bdfaff5b` | Pending Removed Load Requires Leaf List Membership | If a cfs_rq has non-zero pending removed load/util, it must be on leaf_cfs_rq_list | update_blocked_averages, propagate_entity_cfs_rq, detach_entity_cfs_rq |

## Deadline / DL Server

Invariants on SCHED_DEADLINE and DL server: dl_bw accounting, running_bw consistency, dl_server lifecycle, bandwidth rebuild, and timer management.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| deadline_dl_global_validate_bw | `a57415f5d1e4` | DL Root Domain Bandwidth Capacity Invariant | Total allocated DL bandwidth in a root domain must not exceed total available DL bandwidth | __dl_add, sched_dl_global_validate, rebuild_sched_domains |
| deadline_dlserver_double_enqueue | `b53127db1dbf` | DL Entity No Double Enqueue | A `sched_dl_entity` already on the DL runqueue must not be enqueued again | __enqueue_dl_entity |
| deadline_dlserver_early_init | `9f239df55546` | DL extra_bw consistency with root domain total_bw | For every CPU in a root domain, `extra_bw` must equal `max_bw - dl_bw.total_bw / nr_active_cpus` | __dl_add/__dl_sub return, scheduler_tick |
| deadline_dlserver_param_running_bw | `bb4700adc3ab` | DL running_bw Consistency with Entity dl_bw | `dl_rq->running_bw` must equal sum of `dl_se->dl_bw` for all contending entities on that rq | dl_server_apply_params, dequeue_dl_entity, enqueue_dl_entity |
| deadline_dl_server_scaled_runtime | `fc975cfb3639` | dl_server Runtime Uses Wall-Clock Time | For dl_server entities, runtime deducted from dl_se->runtime must equal raw wall-clock delta_exec | update_curr_dl_se, dl_server_update_idle_time |
| deadline_dl_server_stuck | `4ae8d9aa9f9d` | Active dl_server Must Have a Liveness Mechanism | If dl_server is active, it must be enqueued, have a pending timer, or be running | scheduler_tick / task_tick_dl, dl_server_timer |
| deadline_dlserver_time_accounting | `c7f7e9c73178` | Inactive DL Server Must Not Have Runtime Accounting Updated | When fair_server's dl_server_active is false, no path should modify its runtime accounting fields | update_curr, update_curr_task |
| deadline_dl_server_yield_delay | `a3a70caf7906` | DL Server Must Not Yield | A dl_server entity should never have dl_yielded == 1; yielding is incorrect for bandwidth servers | update_curr_dl_se, __pick_task_dl |
| deadline_extra_bw_stale_rebuild | `fcc9276c4d33` | DL Bandwidth Consistency: extra_bw vs total_bw | Sum of reserved BW across all CPUs in a root domain must equal domain-wide total_bw | dl_clear_root_domain, dl_add_task_root_domain |
| deadline_hrtick_enabled_wrong_check | `d16b7eb6f523` | Hrtick Timer Armed Implies Scheduling-Class Feature Enabled | If hrtick timer is active on a runqueue, the class-specific hrtick sched_feat must be enabled | set_next_task_dl, set_next_task_fair |
| deadline_special_task_domain_rebuild | `f6147af176ea` | DL Bandwidth Rebuild Consistency | After domain rebuild, each root domain's `dl_bw.total_bw` must equal sum of dl-server + non-special DL tasks | partition_sched_domains_locked |

## Core Scheduling

Invariants on core scheduling: core_cookie and core tree membership, SMT sibling consistency, and migration-disabled task constraints.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| core_cookie_update_missing_enqueue | `91caa5ae2424` | Core Tree Membership Consistent with Cookie and Runqueue State | If a task has a non-zero `core_cookie` and is queued, it must be in the core scheduling rb-tree | sched_core_update_cookie, enqueue_task, dequeue_task |
| core_delayed_dequeue_core_sched | `c662e2b1e8cf` | Delayed Tasks Must Not Be In Core Tree | If a task has `se.sched_delayed == 1`, it must not be present in the `rq->core_tree` | sched_core_enqueue, enqueue_task, dequeue_task |
| core_rq_lock_uninitialized_core | `3c474b3239f1` | Core Scheduling SMT Sibling Core Pointer Consistency | `rq->core` must be valid non-NULL pointing to same SMT group, all siblings agree on core leader | rq_lockp, sched_core_cpu_starting, sched_core_cpu_deactivate, sched_core_cpu_dying |
| core_steal_cookie_migration_disabled | `386ef214c3c6` | Migration-Disabled Tasks Must Not Change CPU | If a task has `migration_disabled > 0`, `set_task_cpu()` must not change its CPU | set_task_cpu |

## Uclamp

Invariants on utilization clamping: uclamp bounds consistency, rq aggregation, cgroup propagation, capacity fitness, and misfit detection.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| uclamp_asym_fits_capacity | `a2e7f03ed28f` | Uclamp Bounds Must Bypass Migration Margin in CPU Fitness Checks | Uclamp bounds must be compared against CPU's original capacity without fits_capacity() margin | asym_fits_cpu, task_fits_cpu, select_idle_capacity, find_energy_efficient_cpu |
| uclamp_bucket_id_oob | `6d2f8909a5fa` | Uclamp Bucket ID Within Bounds | A task's uclamp bucket_id must always be a valid index in [0, UCLAMP_BUCKETS-1] | uclamp_rq_inc_id, uclamp_rq_dec_id, uclamp_se_set |
| uclamp_cgroup_propagation_missing | `7226017ad37a` | Uclamp Effective Value Hierarchical Consistency | Effective uclamp value must equal min(requested, parent_effective) for any task group | cpu_cgroup_css_online, cpu_util_update_eff |
| uclamp_cpu_overutilized_unaware | `c56ab1b3506b` | Uclamp-Overutilized Consistency | When uclamp is active, CPU with uclamp_max within capacity must not be overutilized | update_overutilized_status |
| uclamp_flag_idle_stale | `ca4984a7dd86` | UCLAMP_FLAG_IDLE Consistency with Active Bucket Tasks | If any UCLAMP_MAX bucket has nonzero task count, UCLAMP_FLAG_IDLE must not be set | uclamp_rq_inc_id, enqueue_task |
| uclamp_fork_reset_rt_boost | `eaf5a92ebde5` | Uclamp Request Consistency with Scheduling Policy | Non-user-defined uclamp request value must equal default for task's scheduling policy | sched_fork, __setscheduler_uclamp |
| uclamp_kthread_stacking_asym | `014ba44e8184` | select_idle_sibling Must Respect Asymmetric Capacity Fitness | On asymmetric CPU capacity systems, select_idle_sibling() must return CPU with sufficient capacity | select_idle_sibling |
| uclamp_migration_margin_fits | `48d5e9daa8b7` | Uclamp-Boosted Task Must Fit Biggest CPU | On the biggest CPU, a task whose actual utilization fits must not be marked as misfit | update_misfit_status, task_tick_fair |
| uclamp_min_limit_not_protection | `0c18f2ecfcc2` | Cgroup uclamp_min Protection Floor | For any task in a non-root, non-autogroup cgroup, effective uclamp_min >= task group's uclamp_min | uclamp_cpu_inc, uclamp_eff_get |
| uclamp_rq_init_zero_max | `d81ae8aac85c` | Uclamp RQ Default Value Consistency | When no tasks contribute to a uclamp clamp_id on a rq, value must equal uclamp_none(clamp_id) | uclamp_rq_inc_id, uclamp_rq_dec_id, scheduler_tick |
| uclamp_rq_max_first_enqueue | `315c4f884800` | Uclamp rq aggregation consistency with enqueued tasks | When rq transitions 0→1 tasks via enqueue, rq->uclamp[clamp_id].value must equal task's effective uclamp | enqueue_task |
| uclamp_task_fits_migration_margin | `b48e16a69792` | Uclamp-Capped Tasks Must Fit CPUs Matching Their Cap | When task's effective uclamp_max ≤ capacity_orig_of(cpu), rq->misfit_task_load must be 0 | update_misfit_status |
| uclamp_tg_restrict_inversion | `0213b7083e81` | Effective Uclamp Min Never Exceeds Effective Uclamp Max | For any task on a runqueue, its effective `uclamp_min` must be ≤ its effective `uclamp_max` | uclamp_rq_inc, uclamp_update_active |

## Task State / Lifecycle

Invariants on task state consistency: on_rq flags, CPU affinity, scheduling class transitions, rq online state, and PSI/trace coherence.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| core_psi_delayed_dequeue_block | `f5aaff7bfa11` | PSI Flags Consistent With Task CPU Execution State | After a blocking context switch, `TSK_RUNNING` must not remain set in prev task's `psi_flags` | psi_sched_switch, finish_task_switch |
| core_ptrace_freeze_state_race | `d136122f5845` | Sleeping task must be dequeued after voluntary schedule | After __schedule() deactivation for non-preempted sleeping task, task must be dequeued (on_rq==0) | __schedule() |
| core_rq_online_hotplug_rollback | `fe7a11c78d2a` | Active CPU Runqueue Online Consistency | For any CPU in `cpu_active_mask`, its runqueue must have `rq->online == 1` | scheduler_tick, sched_cpu_deactivate |
| core_sched_switch_stale_state | `8feb053d5319` | Trace prev_state Consistency With on_rq Status | At `trace_sched_switch`, if not preemption and prev on rq, prev_state must be TASK_RUNNING | trace_sched_switch |
| core_setaffinity_user_cpus_skip | `df14b7f9efcd` | user_cpus_ptr reflects last successful sched_setaffinity | After `__set_cpus_allowed_ptr_locked()` returns success with SCA_USER, `user_cpus_ptr` must match user mask | __set_cpus_allowed_ptr_locked, __sched_setaffinity |
| core_ttwu_stale_cpu_race | `b6e13e85829f` | Task-CPU / Runqueue Consistency on Activation | When a task is activated (enqueued) on a runqueue, `task_cpu(p)` must equal `cpu_of(rq)` | ttwu_do_activate, sched_ttwu_pending, activate_task |
| core_ttwu_wakelist_cpumask | `751d4cbc4387` | Task CPU Affinity Consistency | When a task is activated on a runqueue, the target CPU must be in the task's cpus_ptr mask | ttwu_do_activate, sched_ttwu_pending |
| fair_delayed_dequeue_class_change | `75b6499024a6` | on_rq consistency after scheduling class switch | After a class switch, if task is sleeping and not queued in new class, `p->on_rq` must be 0 | check_class_changed, switched_from_fair |

## Weight / Reweight

Invariants on load.weight consistency across reweight operations: weighted lag preservation, avg_vruntime freshness, and cfs_rq load weight sums.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| cgroup_empty_weight_delay_dequeue | `66951e4860d3` | Delay-Dequeued Group Entity Weight Preservation | A delay-dequeued group sched_entity must not have its `load.weight` changed | reweight_entity, update_cfs_group |
| eevdf_reweight_dequeue_avruntime | `afae8002b4fd` | Reweight Preserves Weighted Lag | Reweight on a CFS entity must preserve weighted lag: `w_new*(V_after-v_after)==w_old*(V_before-v_before)` | reweight_entity |
| eevdf_reweight_placement_lag | `6d71a9c6160479` | Weighted Lag Preservation Across Reweight | When reweight_entity() changes an on_rq entity's weight, absolute weighted lag must be preserved | reweight_entity (exit) |
| eevdf_reweight_stale_avg_vruntime | `11b1b8bc2b98` | Fresh V Before Reweight EEVDF Calculations | When `reweight_eevdf()` is called, curr's execution must be committed via `update_curr()` | reweight_eevdf |
| eevdf_reweight_vruntime_unadjusted | `eab03c23c2a1` | EEVDF Lag Preservation Through Reweight | When an on-rq CFS entity's weight changes, its lag must be preserved | reweight_entity |
| fair_sched_idle_reweight_skip | `d32960528702` | CFS Runqueue Load Weight Consistency | `cfs_rq->load.weight` must equal the sum of `se->load.weight` for all on-rq entities | task_tick_fair, reweight_entity |

## Vruntime / Eligibility

Invariants on vruntime bounds, min_vruntime monotonicity, avg_vruntime consistency, entity eligibility, and lag bounds.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| eevdf_avg_vruntime_truncation | `650cad561cce` | Freshly Placed Entity Must Be Eligible | After place_entity() sets a waking/new entity's vruntime, that entity must satisfy entity_eligible() | enqueue_entity |
| eevdf_pick_eevdf_eligible_miss | `b01db23d5923` | EEVDF Pick Returns Earliest Eligible Deadline | The entity returned by `pick_eevdf()` must have the minimum deadline among all eligible entities | set_next_entity |
| eevdf_reweight_curr_min_vruntime | `5068d84054b7` | min_vruntime Consistency After Vruntime Modification | After modifying an on-rq CFS entity's vruntime, min_vruntime must match update_min_vruntime() | reweight_entity, scheduler_tick |
| eevdf_reweight_vlag_overflow | `1560d1f6eb6b` | Entity Lag Bounded by Slice | Virtual lag of any CFS entity must be bounded by ~2x its time slice in arithmetic | reweight_eevdf, update_entity_lag |
| fair_migrated_entity_vruntime_reset | `a53ce18cacb4` | Wakeup Placement Must Not Decrease Vruntime | After place_entity(cfs_rq, se, 0) for wakeup, se->vruntime must be >= its prior value | enqueue_entity |

## EEVDF Pick & Scheduling

Invariants on EEVDF entity selection, buddy pointers, slice management, rb-tree augmented data, and preemption logic.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| cgroup_idle_preempt_hierarchy | `faa42d29419d` | Idle Cgroup Preemption Consistency | After wakeup preemption check, if curr's matched entity is idle and waking is non-idle, resched must be set | check_preempt_wakeup_fair |
| eevdf_cgroup_min_slice_propagation | `563bc2161b94` | CFS RB-Tree Augmented min_slice Consistency | RB-tree root's augmented min_slice must equal the minimum slice among all entities | enqueue_task_fair, dequeue_entities |
| eevdf_idle_entity_slice_protection | `f553741ac8c0` | SCHED_IDLE Entity Must Not Be Picked Over Non-Idle Eligible Entities | When pick_eevdf() returns a SCHED_IDLE entity, no non-idle eligible entity may exist on same cfs_rq | pick_eevdf |
| eevdf_next_buddy_delayed | `493afbd187c4` | CFS Buddy Pointers Must Not Reference Delayed Entities | CFS buddy pointers (`cfs_rq->next`, `cfs_rq->last`) must never point to delayed entity | pick_next_entity, set_next_buddy/set_last_buddy, dequeue_entity |
| eevdf_nr_running_preempt_check | `d4ac164bde7a` | CFS Lone-Entity No Self-Preempt | When a cfs_rq has exactly one runnable entity, preemption logic must not reschedule | update_curr |
| eevdf_reweight_curr_heap_corruption | `d2929762cc3f` | CFS min_deadline Augmented Heap Invariant | For every CFS rb-tree node, min_deadline must equal min of own deadline and children's min_deadline | reweight_entity, pick_eevdf |
| eevdf_reweight_min_deadline_heap | `8dafa9d0eb1a` | CFS rb-tree min_deadline augmented heap consistency | `se->min_deadline` must equal min of `se->deadline` and children's `min_deadline` | reweight_entity, scheduler_tick, update_curr |
| eevdf_reweight_nr_queued_stale | `c70fc32f4443` | place_entity() Precondition: nr_queued Excludes Placed Entity | When place_entity(cfs_rq, se, flags) is called, nr_queued must not count se | place_entity |
| eevdf_slice_stale_on_placement | `2f2fc17bab00` | Entity Slice Matches Sysctl at Placement | After `place_entity()` completes for a non-custom-slice entity, `se->slice` must equal `sysctl_sched_base_slice` | place_entity, enqueue_entity |
| eevdf_slice_u64max_crash | `bbce3de72be5` | Sched Entity Slice Must Be Bounded | `se->slice` must not exceed a reasonable upper bound (and must never be `U64_MAX`) | enqueue_entity, set_next_entity, dequeue_entities |

## Load Balancing

Invariants on load balancing: imbalance computation, idle CPU selection within domain spans, misfit detection, and balance initiator uniqueness.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| energy_misfit_pinned_balance_interval | `0ae78eec8aa6` | Misfit Status Requires Migratable Task | If `rq->misfit_task_load` is non-zero, the running task must be migratable | update_misfit_status, check_misfit_status / load balancer entry |
| fair_recent_used_cpu_affinity | `ae2ad293d6be` | Selected CPU Must Be In Task's Allowed Affinity Mask | The CPU returned by `select_task_rq()` must be set in the task's `p->cpus_ptr` affinity mask | select_task_rq_fair, __set_task_cpu |
| lb_calculate_imbalance_overflow | `91dcf1e8068e` | Load Balancer Must Not Pull Load to Above-Average Groups | When migrate_load imbalance is computed, local group's avg load must be below system-wide avg | calculate_imbalance, sched_balance_rq/load_balance |
| lb_idle_core_isolcpus | `23d04d8c6b8e` | Idle CPU Selection Must Respect Scheduling Domain Span | Any CPU returned by `select_idle_sibling()` must belong to `sched_domain_span(sd)` | select_idle_sibling, select_idle_core |
| lb_idle_smt_domain_mask | `df3cb4ea1fb6` | select_idle_sibling Must Return CPU Within Scheduling Domain | The CPU returned by `select_idle_sibling()` must belong to the LLC scheduling domain span of the target CPU | select_idle_sibling |
| lb_select_idle_smt_isolcpus | `8aeaffef8c6e` | Idle CPU Selection Must Respect Scheduling Domain Span | Any CPU chosen by `select_idle_sibling()` must belong to `sched_domain_span(sd)` of the LLC domain | select_idle_sibling (return point) |
| lb_should_we_balance_dual_cpu | `6d7e4782bcf5` | Unique Load Balance Initiator Per Scheduling Group | `should_we_balance()` must return true for at most one CPU per periodic balance pass | load_balance() entry |

## RT Scheduling

Invariants on real-time scheduling: RT runqueue membership, priority bitmap–queue consistency, and throttle timer management.

| Bug | Commit | Invariant | Property | Check Point(s) |
|-----|--------|-----------|----------|----------------|
| rt_double_enqueue_setscheduler | `f558c2b834ec` | RT Runqueue Membership Consistent with Scheduling Class | If a task's RT sched_entity is on_list, then sched_class must be rt_sched_class | __sched_setscheduler, enqueue_task_rt |
| rt_period_timer_permanent_throttle | `9b58e976b3b3` | RT Throttled Implies Period Timer Active | If an RT runqueue is throttled, the RT bandwidth period timer must be active | update_curr_rt, task_tick_rt |
| rt_pick_next_empty_queue | `7c4a5b89a0b5` | RT Priority Bitmap–Queue Consistency | If a bit is set in `rt_rq->active.bitmap`, the corresponding priority queue must be non-empty | pick_next_rt_entity, dequeue_rt_entity |


