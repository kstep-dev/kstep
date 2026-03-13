# Planned Driver Reviews

Quality review of each planned driver analysis.

| Driver | Commit | Verdict | Difficulty | Internal State | Notes |
|--------|--------|---------|------------|----------------|-------|
| bandwidth_burst_unit_mismatch | `49217ea147df` | FEASIBLE | EASY | READ_ONLY | Deterministic control-plane bug; cgroup write triggers unit mismatch directly, pattern matches existing drivers |
| cgroup_autogroup_sysctl_missing | `82f586f923e3` | FEASIBLE | EASY | READ_ONLY | Boot-time init bug; filp_open check is deterministic and needs no tasks or cgroups |
| bandwidth_unthrottle_delayed_entity | `9b5ce1a37e90` | FEASIBLE | MEDIUM | READ_ONLY | Thorough analysis with correct root cause; eligibility control is the key challenge, mitigated by three fallback strategies |
| cgroup_cpuacct_wrong_cpu_charge | `248cc9993d1c` | FEASIBLE | MEDIUM | READ_ONLY | Sound approach: cpuacct struct duplication for percpu reads, balance callback for snapshot timing |
| bandwidth_hierarchical_quota_cgroupv2 | `c98c18270be1` | FEASIBLE | EASY | READ_ONLY | Deterministic control-plane bug; cgroup write triggers path directly, no timing or extensions needed |
| bandwidth_unthrottle_zero_runtime | `956dfda6a708` | FEASIBLE | MEDIUM | READ_ONLY | Accurate plan validated by existing driver; cgroup+tick orchestration, no extensions needed |
| bandwidth_pelt_clock_sync_throttle | `0e4a169d1a2b` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Correct analysis but wrongly claims kSTEP extensions needed; cgroup_write and hierarchy already exist |
| bandwidth_csd_unthrottle_double_clock | `ebb83d84e49b` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Plan wrongly suggests needing API extension; kstep_cgroup_write already sets cpu.max |
| cgroup_empty_weight_delay_dequeue | `66951e4860d3` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; ensure positive lag for DELAY_DEQUEUE before blocking tasks |
| bandwidth_throttle_block_delayed | `e67e3e738f08` | FEASIBLE | MEDIUM | READ_ONLY | Thorough analysis; KSYM force-dequeue mirrors wait_task_inactive path correctly |
| cgroup_fork_stale_task_group | `4ef0c5c6b5ba` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Race needs probabilistic loop; no deterministic hook between dup_task_struct and sched_fork |
| cgroup_dead_tg_cfsrq_uaf | `b027789e5e50` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Needs kstep_cgroup_destroy extension; race-window timing in deterministic kSTEP env is challenging |
| cgroup_idle_preempt_hierarchy | `faa42d29419d` | FEASIBLE | EASY | READ_ONLY | Deterministic control-plane bug; all APIs exist, straightforward cgroup+task setup triggers it directly |
| bandwidth_unthrottle_leaf_ancestor | `2630cde26711` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | PELT won't decay while throttled; must arrange differential decay before throttle |
| cgroup_leaf_list_container_of | `3b4035ddbfc8` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Sound approach reaches bug path; pass/fail may not distinguish kernels due to layout-dependent garbage |
| core_cpus_share_cache_race | `42dc938a590c` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Instruction-level race; probabilistic approach unlikely to trigger reliably in QEMU |
| core_delayed_dequeue_core_sched | `c662e2b1e8cf` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Accurate analysis; needs CONFIG_SCHED_CORE and direct core_cookie writes with internal locking |
| core_nr_uninterruptible_overflow | `36569780b0d6` | FEASIBLE | EASY | WRITES_INTERNALS | Direct rq->nr_uninterruptible write needed; deterministic type-size bug with clear pass/fail |
| core_cookie_update_missing_enqueue | `91caa5ae2424` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Sound approach via sched_core_share_pid; plan needs to commit to one method; verify CONFIG_SCHED_CORE |
| core_psi_delayed_dequeue_block | `f5aaff7bfa11` | FEASIBLE | EASY | READ_ONLY | Mirrors existing freeze.c pattern; straightforward block-wake with psi_flags read check |
| core_preempt_dynamic_none_str | `3ebb1b652239` | FEASIBLE_WITH_CHANGES | EASY | WRITES_INTERNALS | Directly writes preempt_dynamic_mode; use CONFIG_PREEMPT_NONE=y kernel config instead |
| core_nohz_ksoftirqd_wakeup | `e932c4ab38f0` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Needs idle=poll boot param and nohz state init fix; complex nohz path prerequisites |
| core_forceidle_balance_wrong_context | `5b6547ed97f4` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Requires CONFIG_SCHED_CORE; writes core_cookie; race timing hard to make deterministic |
| core_irqtime_static_key | `f3fa0e40df17` | FEASIBLE | EASY | NONE | Simple direct call from atomic context; needs CONFIG_DEBUG_ATOMIC_SLEEP for warning visibility |
| core_sched_switch_stale_state | `8feb053d5319` | FEASIBLE | MEDIUM | READ_ONLY | Sound tracepoint-hook approach; needs custom kthread and version-guarded tracepoint signatures |
| core_stop_tick_cgroup_nr_running | `c1f43c342e1f` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Sound strategy but requires CONFIG_NO_HZ_FULL kernel rebuild; deterministic data-condition bug |
| core_setaffinity_cpuset_warn | `70ee7947a290` | FEASIBLE | MEDIUM | READ_ONLY | Correct race analysis; needs raw kthreads and KSYM_IMPORT(sched_setaffinity) for the SCA_USER path |
| core_task_call_func_race | `91dabf33ae5d` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Statistical race reproduction is inherently non-deterministic; may need many iterations in QEMU |
| core_wait_task_inactive_delayed | `b7ca5743a260` | FEASIBLE | EASY | READ_ONLY | Clean timing-based reproducer using exported kthread_bind(); no extensions needed |
| core_yield_to_ttwu_race | `5d808c78d972` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Probabilistic race with tiny window; needs delay injection or hook to trigger deterministically in QEMU |
| deadline_deboosted_bugon_condition | `ddfc710395cc` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Needs rt_mutex+DL kSTEP extensions; fallback approach writes pi_se directly |
| core_setaffinity_user_cpus_skip | `df14b7f9efcd` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Correct root cause; scenario self-corrects mid-analysis; needs KSYM_IMPORT for sched_setaffinity |
| core_ttwu_wakelist_cpumask | `751d4cbc4387` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Microsecond race window needs spinning waker kSTEP extension; reproduction may lack determinism |
| deadline_dlserver_early_init | `9f239df55546` | FEASIBLE | EASY | READ_ONLY | Boot-time state observation only; accurate analysis, deterministic check of extra_bw values |
| deadline_dlserver_param_running_bw | `bb4700adc3ab` | FEASIBLE | EASY | READ_ONLY | Deterministic debugfs write during lazy-stop window; all APIs available, simple sequence |
| deadline_dlserver_nr_running_doublecount | `52d15521eb75` | FEASIBLE | EASY | READ_ONLY | Deterministic accounting bug; wake CFS task on idle CPU, read nr_running for double-count |
| deadline_dlserver_schedstats_crash | `9c602adb799e` | FEASIBLE_WITH_CHANGES | EASY | NONE | Deterministic BUG_ON; trivial trigger but requires CONFIG_SCHEDSTATS=y kernel build change |
| deadline_dl_server_scaled_runtime | `fc975cfb3639` | FEASIBLE | MEDIUM | READ_ONLY | Deterministic scaling bug; kSTEP freq/capacity APIs directly trigger the buggy code path |
| deadline_dlserver_double_enqueue | `b53127db1dbf` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Sound Case 1 approach; blocking sequence may need tuning for delayed dequeue path |
| deadline_dl_server_stuck | `4ae8d9aa9f9d` | FEASIBLE | MEDIUM | READ_ONLY | Deterministic via tick_until; block CFS before dl_timer fires to trigger stuck dl_server |
| deadline_dl_server_start_stop_overhead | `cccb45d7c429` | FEASIBLE | EASY | READ_ONLY | Clean state-observation approach; block CFS task and read dl_server_active to detect aggressive vs lazy stop |
| deadline_dlserver_timer_starvation | `421fc59cf58c` | FEASIBLE_WITH_CHANGES | MEDIUM | WRITES_INTERNALS | Accurate analysis; requires dl_server field writes since kSTEP cannot simulate heavy IRQ load |
| deadline_dl_server_stopped_inversion | `4717432dfd99` | FEASIBLE | MEDIUM | READ_ONLY | Well-analyzed; task block + dl_server period cycling should trigger the inverted return path |
| deadline_extra_bw_stale_rebuild | `fcc9276c4d33` | FEASIBLE | EASY | READ_ONLY | Deterministic control-plane bug; topo_apply triggers rebuild and extra_bw drift is directly observable |
| deadline_dl_timer_refcount_leak | `b58652db66c9` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Correct analysis; needs 2 kSTEP extensions (DL policy + rt_mutex actions) and 3-task/2-mutex orchestration |
| deadline_hrtick_enabled_wrong_check | `d16b7eb6f523` | FEASIBLE | EASY | READ_ONLY | Deterministic one-line check bug; DL task + hrtimer_active observation suffices |
| deadline_dl_server_yield_delay | `a3a70caf7906` | FEASIBLE | MEDIUM | READ_ONLY | Deterministic block/wake cycle triggers yield path; standard APIs suffice |
| deadline_empty_cpuset_crash | `b6e8d40d43ae` | FEASIBLE | EASY | NONE | Deterministic control-plane crash; cgroup write with empty cpuset triggers OOB per-CPU access |
| deadline_dlserver_time_accounting | `c7f7e9c73178` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Companion fix b53127db already applied at ~1; Case 1 dl_yielded/dl_throttled detection won't trigger |
| deadline_special_task_domain_rebuild | `f6147af176ea` | FEASIBLE | MEDIUM | READ_ONLY | Deterministic cpuset-triggered dl_bw inflation; strategy is sound using sched_setattr_nocheck + cpuset rebuild |
| eevdf_h_nr_delayed_group_entity | `3429dd57f0de` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; existing h_nr_runnable.c driver already reproduces this same commit's bug |
| deadline_rd_bw_accounting_rebuild | `2ff899e35164` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Needs DL task extension, cpuset partition type config, and cpuset_mutex locking for KSYM call |
| eevdf_avg_vruntime_truncation | `650cad561cce` | FEASIBLE | EASY | READ_ONLY | Accurate analysis; existing avg_vruntime_ceil.c driver already implements this exact strategy |
| eevdf_nr_running_preempt_check | `d4ac164bde7a` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; cgroup hierarchy approach deterministically triggers the nr_running mismatch |
| eevdf_idle_entity_slice_protection | `f553741ac8c0` | FEASIBLE | EASY | READ_ONLY | Clean deterministic plan; simple 2-task setup with SCHED_IDLE policy triggers the bug path directly |
| deadline_migrate_enable_boosted_warn | `0664e2c311b9` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Needs custom kthread actions for rt_mutex PI; timing orchestration across 3 entities is complex |
| eevdf_delayed_dequeue_ttwu_race | `b55945c500c5` | INFEASIBLE | HARD | READ_ONLY | Few-instruction SMP race window cannot be deterministically triggered at kSTEP tick granularity |
| eevdf_pick_eevdf_eligible_miss | `b01db23d5923` | FEASIBLE | MEDIUM | READ_ONLY | Brute-force EED verification is sound; tree topology triggering is probabilistic but manageable |
| eevdf_reweight_min_deadline_heap | `8dafa9d0eb1a` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; existing min_deadline.c driver already validates approach with RB-tree walk |
| eevdf_reweight_dequeue_avruntime | `afae8002b4fd` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; cgroup reweight + lag comparison is sound and follows existing driver patterns |
| eevdf_cgroup_min_slice_propagation | `563bc2161b94` | FEASIBLE | MEDIUM | READ_ONLY | Well-analyzed; may need extra cgroup entity to ensure group_se != cfs_rq->curr |
| eevdf_reweight_curr_min_vruntime | `5068d84054b7` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; cgroup reweight on tick deterministically hits the buggy curr path |
| eevdf_next_buddy_delayed | `493afbd187c4` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Load-balancing trigger is non-deterministic; simpler same-CPU wakeup of delayed entity suffices |
| eevdf_reweight_nr_queued_stale | `c70fc32f4443` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Bug masked by se->vlag optimization; needs new kSTEP callback to observe intermediate nr_queued state |
| eevdf_reweight_stale_avg_vruntime | `11b1b8bc2b98` | FEASIBLE | MEDIUM | READ_ONLY | Thorough analysis; cgroup weight change triggers reweight deterministically like existing drivers |
| eevdf_reweight_placement_lag | `6d71a9c61604` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; cgroup weight cycling pattern well-suited; mirrors existing vlag_overflow driver |
| eevdf_reweight_curr_heap_corruption | `d2929762cc3f` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Thorough analysis; PELT-based share trigger may need multiple tick cycles to reliably fire |
| eevdf_reweight_vlag_overflow | `1560d1f6eb6b` | FEASIBLE | MEDIUM | READ_ONLY | Correct analysis but duplicate of existing vlag_overflow.c driver targeting same commit |
| eevdf_reweight_vruntime_unadjusted | `eab03c23c2a1` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; deterministic trigger via set_prio with clear vruntime before/after check |
| eevdf_slice_stale_on_placement | `2f2fc17bab00` | FEASIBLE | EASY | READ_ONLY | Accurate analysis; existing slice_update.c driver already implements this exact strategy |
| eevdf_slice_u64max_crash | `bbce3de72be5` | FEASIBLE | HARD | READ_ONLY | Accurate analysis; orchestrating nested delayed dequeue cascade requires precise timing |
| energy_feec_uclamp_max_zero | `23c9519def98` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Requires CONFIG_ENERGY_MODEL=y and new kstep_em_register() helper; EAS setup on x86 QEMU is complex |
| energy_rd_overutilized_wrong_flag | `cd18bec668bb` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Needs CONFIG_ENERGY_MODEL + static_branch_enable hack for EAS; feasible but setup is complex |
| energy_feec_uclamp_min_skip | `d81304bc6193` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Requires Energy Model registration kSTEP extension; no EM infra exists yet |
| fair_delayed_dequeue_class_change | `75b6499024a6` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; proven ineligibility pattern from freeze.c triggers sched_delayed reliably |
| fair_migrated_entity_vruntime_reset | `a53ce18cacb4` | FEASIBLE | MEDIUM | READ_ONLY | Deterministic bug; clock advance + block/migrate sequence triggers exec_start=0 path reliably |
| fair_percpu_kthread_wakeup_context | `8b4e74ccb582` | FEASIBLE | MEDIUM | READ_ONLY | Sound IPI-based interrupt-context wakeup approach; no kSTEP extensions needed |
| fair_sched_idle_reweight_skip | `d32960528702` | FEASIBLE | EASY | READ_ONLY | Deterministic bug; trivial kstep_task_idle extension triggers skipped reweight_task path |
| fair_recent_used_cpu_affinity | `ae2ad293d6be` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; kthread bind/block/wake sequence clearly triggers wrong-CPU affinity check |
| fair_wake_affine_delayed_dequeue | `aa3ee4f0b754` | FEASIBLE | MEDIUM | READ_ONLY | Thorough analysis; approach mirrors existing sync_wakeup.c driver for same commit |
| lb_detach_tasks_pinned_loop | `2feab2492deb` | FEASIBLE | EASY | READ_ONLY | Deterministic O(n) loop bug; existing long_balance.c driver already implements this exact approach |
| fair_ttwu_move_affine_miscount | `39afe5d6fc59` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | CPU 0 won't be idle during wakeup; needs waker kthread on separate CPU to trigger wake_affine path |
| lb_calculate_imbalance_overflow | `91dcf1e8068e` | FEASIBLE | MEDIUM | READ_ONLY | Sound approach using cgroup bandwidth throttling; PELT convergence timing may need tuning |
| lb_should_we_balance_dual_cpu | `6d7e4782bcf5` | FEASIBLE | EASY | READ_ONLY | Accurate analysis; existing extra_balance.c already implements same bug with identical approach |
| lb_ilb_spurious_need_resched | `ff47a0acfcce` | FEASIBLE_WITH_CHANGES | MEDIUM | WRITES_INTERNALS | Recommended strategy writes TIF_NEED_RESCHED directly; idle=poll approach needs framework extension |
| lb_smt4_group_smt_balance | `450e749707bc` | FEASIBLE | MEDIUM | READ_ONLY | Thorough analysis; SMT4 topology triggers deterministic busiest-selection logic bug cleanly |
| lb_select_idle_smt_isolcpus | `8aeaffef8c6e` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Needs SMT topology + isolcpus=domain boot extensions to run.py; driver logic itself is straightforward |
| pelt_attach_load_sum_zero | `40f5aa4c5eae` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; cgroup migration cleanly triggers attach path; decay loop handles timing |
| pelt_cfs_rq_h_nr_delayed | `76f2f783294d` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; similar to existing h_nr_runnable driver; needs eligibility check before blocking |
| numa_imbalance_group_weight | `2cfb7a1b031b` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Correct analysis but 16-CPU NUMA setup untested in kSTEP; balance interval timing may cause flakiness |
| lb_idle_core_isolcpus | `23d04d8c6b8e` | FEASIBLE_WITH_CHANGES | MEDIUM | WRITES_INTERNALS | Needs isolcpus=domain boot param in run.py; may need to force has_idle_cores flag |
| pelt_hw_pressure_clock_mismatch | `84d265281d6c` | FEASIBLE | MEDIUM | READ_ONLY | Sound plan; needs CONFIG_SCHED_HW_PRESSURE enabled and natural load-balance softirq firing |
| pelt_lost_idle_time_slow_path | `17e3e88ed0b6` | FEASIBLE | EASY | READ_ONLY | Accurate analysis; deterministic control-flow bug triggered by RT task blocking on empty CFS rq |
| pelt_tg_load_avg_stale_propagate | `02da26ad5ed6` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; deterministic cgroup decay path triggers stale tg->load_avg cleanly |
| pelt_util_sum_sync_loss | `98b0d890220d` | FEASIBLE | MEDIUM | READ_ONLY | Deterministic PELT arithmetic bug; cgroup migration triggers hard sync detectable via util_sum check |
| pelt_throttled_clock_mismatch | `64eaf50731ac` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Plan claims kSTEP extension needed for bandwidth; kstep_cgroup_write() already exists |
| rt_period_timer_permanent_throttle | `9b58e976b3b3` | FEASIBLE | EASY | READ_ONLY | Accurate analysis; deterministic sysctl-triggered bug already implemented as rt_runtime_toggle.c |
| pelt_propagate_load_sum_desync | `7c7ad626d9a0` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Sound plan; should use existing kstep_cgroup_write for bandwidth instead of proposing extension |
| rt_push_migrate_disable_race | `feffe5bb274d` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Probabilistic race in double_lock_balance window; needs kthread extension for migrate_disable toggle |
| rt_dequeue_tick_reenable | `5c66d1b9b30f` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Requires enabling CONFIG_NO_HZ_FULL=y in kernel config; boot param support already exists |
| rt_rr_timeslice_rounding | `c7fcb99877f9` | FEASIBLE | EASY | READ_ONLY | Compile-time init bug; trivial driver but requires CONFIG_HZ_300 kernel build config |
| topology_numa_cpuless_node_crash | `617f2c38cb5c` | FEASIBLE_WITH_CHANGES | MEDIUM | NONE | Needs QEMU NUMA config extension in run.py; driver itself is trivial once topology is set up |
| topology_numa_find_nth_null | `5ebf512f3350` | FEASIBLE | EASY | NONE | Single exported API call with out-of-range index triggers NULL deref deterministically |
| uclamp_cpu_overutilized_unaware | `c56ab1b3506b` | FEASIBLE | EASY | READ_ONLY | Thorough analysis; mirrors uclamp_inversion.c pattern with asymmetric capacity setup |
| rt_setprio_push_task_race | `49bef33e4b87` | FEASIBLE_WITH_CHANGES | HARD | WRITES_INTERNALS | Race needs IPI timing kSTEP can't control; needs kthread migrate_disable extension |
| topology_relax_domain_off_by_one | `a1fd0b9d751f` | FEASIBLE | EASY | READ_ONLY | Deterministic off-by-one; kstep_cgroup_write + sd->flags check, no extensions needed |
| uclamp_flag_idle_stale | `ca4984a7dd86` | FEASIBLE | EASY | READ_ONLY | Deterministic cgroup-triggered bug; mirrors existing uclamp_inversion pattern closely |
| uclamp_rq_max_first_enqueue | `315c4f884800` | FEASIBLE | EASY | READ_ONLY | Thorough analysis; deterministic init bug with clear trigger via existing uclamp APIs |
| uclamp_migration_margin_fits | `48d5e9daa8b7` | FEASIBLE_WITH_CHANGES | MEDIUM | READ_ONLY | Patch 1/9 only adds util_fits_cpu(); fixed kernel must include full series for behavioral change |
| uclamp_tg_restrict_inversion | `0213b7083e81` | FEASIBLE | EASY | READ_ONLY | Deterministic cgroup uclamp logic bug; existing driver covers same commit, plan adds both cases |
| uclamp_task_fits_migration_margin | `b48e16a69792` | FEASIBLE | MEDIUM | READ_ONLY | Accurate analysis; deterministic trigger via asymmetric topology + uclamp cap + PELT ramp-up |
| uclamp_feec_fits_capacity | `244226035a1f` | FEASIBLE_WITH_CHANGES | HARD | READ_ONLY | Requires new EM registration kSTEP extension and CONFIG_ENERGY_MODEL; no EAS infra exists yet |
| uclamp_select_idle_capacity_margin | `b759caa1d9f6` | INFEASIBLE | MEDIUM | READ_ONLY | Requires asymmetric CPU caps; kstep_cpu_set_capacity panics on x86 pre-6.12 |
