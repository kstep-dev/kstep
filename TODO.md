# kSTEP Bug Reproduction TODO

**Total: 53/400 reproduced**

---

## core (87)

- [x] `009836b4fa52` sched/core: Fix migrate_swap() vs. hotplug — [`009836b4_fix_migrate_swap_vs_hotplug.md`](bugs/009836b4_fix_migrate_swap_vs_hotplug.md) <!-- driver:migrate_swap_hotplug attempts:1 -->
- [x] `0213b7083e81` sched/uclamp: Fix uclamp_tg_restrict() — [`0213b708_fix_uclamp_tg_restrict.md`](bugs/0213b708_fix_uclamp_tg_restrict.md) <!-- driver:uclamp_inversion attempts:1 -->
- [x] `04193d590b39` sched: Fix balance_push() vs __sched_setscheduler() — [`04193d59_fix_balance_push_vs_sched.md`](bugs/04193d59_fix_balance_push_vs_sched.md) <!-- driver:balance_push_splice attempts:1 -->
- [x] `0ec8d5aed4d1` sched/core: Add WARN_ON_ONCE() to check overflow for migrate_disable() — [`0ec8d5ae_add_warn_on_once_to.md`](bugs/0ec8d5ae_add_warn_on_once_to.md) <!-- driver:migrate_overflow attempts:1 -->
- [x] `11c7aa0ddea8` rq-qos: fix missed wake-ups in rq_qos_throttle try two — [`11c7aa0d_fix_missed_wake_ups_in.md`](bugs/11c7aa0d_fix_missed_wake_ups_in.md) <!-- driver:rq_qos_wakeup attempts:1 -->
- [x] `1293771e4353` sched: Fix migration_cpu_stop() WARN — [`1293771e_fix_migration_cpu_stop_warn.md`](bugs/1293771e_fix_migration_cpu_stop_warn.md) <!-- driver:migration_cpu_stop_warn attempts:1 -->
- [x] `156ec6f42b8d` sched/features: Fix hrtick reprogramming — [`156ec6f4_fix_hrtick_reprogramming.md`](bugs/156ec6f4_fix_hrtick_reprogramming.md) <!-- driver:hrtick_reprogram attempts:1 -->
- [-] `15faafc6b449` sched,init: Fix DEBUG_PREEMPT vs early boot — [`15faafc6_init_fix_debug_preempt_vs.md`](bugs/15faafc6_init_fix_debug_preempt_vs.md) <!-- skipped:boot-time-only-init-task-flag-management-identical-post-boot-state attempts:1 -->
- [x] `1862d8e264de` sched: Fix faulty assertion in sched_change_end() — [`1862d8e2_fix_faulty_assertion_in_sched.md`](bugs/1862d8e2_fix_faulty_assertion_in_sched.md) <!-- driver:sched_change_assert attempts:1 -->
- [x] `223baf9d17f2` sched: Fix performance regression introduced by mm_cid — [`223baf9d_performance_regression_introduced_by_mm.md`](bugs/223baf9d_performance_regression_introduced_by_mm.md) <!-- driver:mm_cid_perf attempts:1 -->
- [x] `234a503e670b` sched: Reject CPU affinity changes based on task_cpu_possible_mask() — [`234a503e_reject_cpu_affinity_changes_based.md`](bugs/234a503e_reject_cpu_affinity_changes_based.md) <!-- driver:reject_affinity attempts:1 -->
- [x] `248cc9993d1c` sched/cpuacct: Fix charge percpu cpuusage — [`248cc999_charge_percpu_cpuusage.md`](bugs/248cc999_charge_percpu_cpuusage.md) <!-- driver:charge_percpu_cpuusage attempts:1 -->
- [x] `28156108fecb` sched: Fix the check of nr_running at queue wakelist — [`28156108_the_check_of_nr_running.md`](bugs/28156108_the_check_of_nr_running.md) <!-- driver:nr_running_wakelist attempts:1 -->
- [-] `29b306c44eb5` PCI: Flush PCI probe workqueue on cpuset isolated partition change — [`29b306c4_pci_flush_pci_probe_workqueue.md`](bugs/29b306c4_pci_flush_pci_probe_workqueue.md) <!-- skipped:bug requires PCI device probing (pci_call_probe/schedule_work_on) which cannot be triggered from kSTEP drivers attempts:1 -->
- [x] `2cab4bd024d2` sched/debug: Fix the runnable tasks output — [`2cab4bd0_the_runnable_tasks_output.md`](bugs/2cab4bd0_the_runnable_tasks_output.md) <!-- driver:sched_debug_output attempts:1 -->
- [-] `399ced9594df` rcu/tasks: Fix stale task snapshot for Tasks Trace — [`399ced95_rcu_tasks_fix_stale_task.md`](bugs/399ced95_rcu_tasks_fix_stale_task.md) <!-- skipped:memory-ordering-bug-only-on-weakly-ordered-archs-not-x86-TSO attempts:1 -->
- [x] `3a7956e25e1d` kthread: Fix PF_KTHREAD vs to_kthread() race — [`3a7956e2_kthread_fix_pf_kthread_vs.md`](bugs/3a7956e2_kthread_fix_pf_kthread_vs.md) <!-- driver:pf_kthread_race attempts:1 -->
- [x] `3ebb1b652239` Fix preemption string of preempt_dynamic_none <!-- driver:preempt_str attempts:1 --> — [`3ebb1b65_fix_preemption_string_of_preempt.md`](bugs/3ebb1b65_fix_preemption_string_of_preempt.md)
- [x] `42dc938a590c` sched/core: Mitigate race cpus_share_cache()/update_top_cache_domain() — [`42dc938a_mitigate_race_cpus_share_cache.md`](bugs/42dc938a_mitigate_race_cpus_share_cache.md) <!-- driver:cpus_share_cache attempts:1 -->
- [-] `494dcdf46e5c` sched: Fix build warning without CONFIG_SYSCTL — [`494dcdf4_fix_build_warning_without_config.md`](bugs/494dcdf4_fix_build_warning_without_config.md) <!-- skipped:build-time-compiler-warning-not-runtime-bug attempts:1 -->
- [-] `505da6689305` sched/clock: Avoid false sharing for sched_clock_irqtime — [`505da668_sched_clock_avoid_false_sharing.md`](bugs/505da668_sched_clock_avoid_false_sharing.md) <!-- skipped:boot-time-init-ordering-bug-cannot-reproduce-in-kstep attempts:1 -->
- [x] `5324953c06bd` Fix wakeup_preempt's next_class tracking — [`5324953c_fix_wakeup_preempts_next_class.md`](bugs/5324953c_fix_wakeup_preempts_next_class.md) <!-- driver:wakeup_next_class attempts:1 -->
- [x] `565790d28b1e` sched: Fix balance_callback() — [`565790d2_fix_balance_callback.md`](bugs/565790d2_fix_balance_callback.md) <!-- driver:fix_balance_callback attempts:1 -->
- [-] `5657c1167835` sched/core: Fix NULL pointer access fault in sched_setaffinity() with non-SMP configs — [`5657c116_fix_null_pointer_access_fault.md`](bugs/5657c116_fix_null_pointer_access_fault.md) <!-- skipped:bug requires non-SMP kernel (CONFIG_SMP=n) which is incompatible with kSTEP's SMP requirement attempts:1 -->
- [-] `58c644ba512c` sched/idle: Fix arch_cpu_idle() vs tracing — [`58c644ba_schedidle_fix_arch_cpu_idle_tracing.md`](bugs/58c644ba_schedidle_fix_arch_cpu_idle_tracing.md) <!-- skipped:requires CONFIG_TRACE_IRQFLAGS/LOCKDEP to manifest; tracing infra bug not scheduler logic attempts:1 -->
- [x] `5aec788aeb8e` sched: Fix TASK_state comparisons — [`5aec788a_task_state_comparisons.md`](bugs/5aec788a_task_state_comparisons.md) <!-- driver:task_state_cmp attempts:1 -->
- [x] `5b6547ed97f4` sched/core: Fix forceidle balancing — [`5b6547ed_forceidle_balancing.md`](bugs/5b6547ed_forceidle_balancing.md) <!-- driver:forceidle_balancing attempts:1 -->
- [x] `5bc78502322a` sched: fix exit_mm vs membarrier (v4) — [`5bc78502_exit_mm_membarrier_v4.md`](bugs/5bc78502_exit_mm_membarrier_v4.md) <!-- driver:exit_mm_membarrier attempts:1 -->
- [x] `5d808c78d972` Fix race between yield_to() and try_to_wake_up() — [`5d808c78_race_between_yield_to_try_to_wake_up.md`](bugs/5d808c78_race_between_yield_to_try_to_wake_up.md) <!-- driver:yield_to_race attempts:1 -->
- [x] `5ebde09d9170` sched/core: Fix RQCF_ACT_SKIP leak — [`5ebde09d_rqcf_act_skip_leak.md`](bugs/5ebde09d_rqcf_act_skip_leak.md) <!-- driver:rqcf_act_skip_leak attempts:1 -->
- [-] `6080fb211672` sched/debug: Fix updating of ppos on server write ops — [`6080fb21_updating_ppos_server_write_ops.md`](bugs/6080fb21_updating_ppos_server_write_ops.md) <!-- skipped:vfs-level-ppos-bug-requires-userspace-file-writes-not-reproducible-via-kstep attempts:1 -->
- [-] `6c125b85f3c8` sched: Export hidden tracepoints to modules — [`6c125b85_export_hidden_tracepoints_modules.md`](bugs/6c125b85_export_hidden_tracepoints_modules.md) <!-- skipped:compile-time-symbol-export-issue-not-runtime-scheduler-bug attempts:1 -->
- [x] `6d2f8909a5fa` sched: Fix out-of-bound access in uclamp — [`6d2f8909_outofbound_access_uclamp.md`](bugs/6d2f8909_outofbound_access_uclamp.md) <!-- driver:uclamp_oob attempts:1 -->
- [-] `6d337eab041d` sched: Fix migrate_disable() vs set_cpus_allowed_ptr() — [`6d337eab_migrate_disable_set_cpus_allowed_ptr.md`](bugs/6d337eab_migrate_disable_set_cpus_allowed_ptr.md) <!-- skipped:requires CONFIG_PREEMPT_RT which depends on ARCH_SUPPORTS_RT not available for x86 at v5.10-rc1 attempts:1 -->
- [x] `703066188f63` sched/fair: Null terminate buffer when updating tunable_scaling — [`70306618_null_terminate_buffer_when_updating.md`](bugs/70306618_null_terminate_buffer_when_updating.md) <!-- driver:null_term_scaling attempts:1 -->
- [x] `70ee7947a290` sched: fix warning in sched_setaffinity — [`70ee7947_fix_warning_in_schedsetaffinity.md`](bugs/70ee7947_fix_warning_in_schedsetaffinity.md) <!-- driver:setaffinity_warn attempts:1 -->
- [x] `76f970ce51c8` Revert "sched/core: Reduce cost of sched_move_task when config autogroup" — [`76f970ce_schedcore_reduce_cost_of_schedmovetask.md`](bugs/76f970ce_schedcore_reduce_cost_of_schedmovetask.md) <!-- driver:sched_move attempts:1 -->
- [-] `771fac5e26c1` Revert "cpufreq: CPPC: Add support for frequency invariance" — [`771fac5e_cpufreq_cppc_add_support_for.md`](bugs/771fac5e_cpufreq_cppc_add_support_for.md) <!-- skipped:bug is in cpufreq/cppc driver not scheduler; scheduler change is only EXPORT_SYMBOL removal attempts:1 -->
- [x] `77d7dc8bef48` sched/mmcid: Revert the complex CID management — [`77d7dc8b_the_complex_cid_management.md`](bugs/77d7dc8b_the_complex_cid_management.md) <!-- driver:mmcid_compact attempts:1 -->
- [-] `7e406d1ff39b` sched: Avoid double preemption in __cond_resched_*lock*() — [`7e406d1f_avoid_double_preemption_in_condreschedlock.md`](bugs/7e406d1f_avoid_double_preemption_in_condreschedlock.md) <!-- skipped:requires CONFIG_PREEMPT=y but kSTEP uses PREEMPT_NONE; double schedule is overhead-only with no observable state difference attempts:1 -->
- [x] `7fb3ff22ad87` sched/core: Fix arch_scale_freq_tick() on tickless systems — [`7fb3ff22_archscalefreqtick_on_tickless_systems.md`](bugs/7fb3ff22_archscalefreqtick_on_tickless_systems.md) <!-- driver:freq_tick attempts:1 -->
- [x] `8039e96fcc1d` Fix forced idle sibling starvation corner case — [`8039e96f_forced_idle_sibling_starvation_corner.md`](bugs/8039e96f_forced_idle_sibling_starvation_corner.md) <!-- driver:forceidle_starvation attempts:1 -->
- [x] `8061b9f5e111` sched/debug: Change need_resched warnings to pr_err — [`8061b9f5_change_needresched_warnings_to_prerr.md`](bugs/8061b9f5_change_needresched_warnings_to_prerr.md) <!-- driver:resched_warn attempts:1 -->
- [x] `82c387ef7568` sched/core: Prevent rescheduling when interrupts are disabled — [`82c387ef_prevent_rescheduling_when_interrupts_are.md`](bugs/82c387ef_prevent_rescheduling_when_interrupts_are.md) <!-- driver:cond_resched_irq attempts:1 -->
- [x] `87ca4f9efbd7` Fix use-after-free bug in dup_user_cpus_ptr() — [`87ca4f9e_fix_useafterfree_bug_dup_user.md`](bugs/87ca4f9e_fix_useafterfree_bug_dup_user.md) <!-- driver:dup_user_cpus_uaf attempts:1 -->
- [x] `8a6edb5257e2` sched: Fix migration_cpu_stop() requeueing — [`8a6edb52_fix_migration_cpu_stop_requeueing.md`](bugs/8a6edb52_fix_migration_cpu_stop_requeueing.md) <!-- driver:migration_cpu_stop_requeue attempts:1 -->
- [-] `8ba09b1dc131` sched: print stack trace with KERN_INFO — [`8ba09b1d_print_stack_trace_with_kern.md`](bugs/8ba09b1d_print_stack_trace_with_kern.md) <!-- skipped:cosmetic log-level change (show_stack→show_stack_loglvl); no scheduler behavior impact; not reproducible via kSTEP attempts:1 -->
- [x] `8d4d9c7b4333` sched/debug: Fix memory corruption caused by multiple small reads of flags — [`8d4d9c7b_fix_memory_corruption_caused_multiple.md`](bugs/8d4d9c7b_fix_memory_corruption_caused_multiple.md) <!-- driver:sched_debug_flags_corrupt attempts:2 -->
- [-] `8d737320166b` sched: Fix build for modules using set_tsk_need_resched() — [`8d737320_fix_build_for_modules_using.md`](bugs/8d737320_fix_build_for_modules_using.md) <!-- skipped:build-time-only-bug-missing-EXPORT_SYMBOL_GPL-not-runtime-reproducible attempts:1 -->
- [x] `8feb053d5319` sched: Fix trace_sched_switch(.prev_state) — [`8feb053d_fix_trace_sched_switch_prev.md`](bugs/8feb053d_fix_trace_sched_switch_prev.md) <!-- driver:sched_switch_prev_state attempts:1 -->
- [x] `91caa5ae2424` Fix the bug that task won't enqueue into core tree when update cookie — [`91caa5ae_fix_bug_that_task_wont.md`](bugs/91caa5ae_fix_bug_that_task_wont.md) <!-- driver:core_enqueue attempts:1 -->
- [x] `91dabf33ae5d` sched: Fix race in task_call_func() — [`91dabf33_fix_race_task_call_func.md`](bugs/91dabf33_fix_race_task_call_func.md) <!-- driver:task_call_func_race attempts:1 -->
- [x] `942b8db96500` sched: Fix migrate_disable_switch() locking — [`942b8db9_fix_migrate_disable_switch_locking.md`](bugs/942b8db9_fix_migrate_disable_switch_locking.md) <!-- driver:migrate_disable_lock attempts:1 -->
- [x] `96500560f0c7` Avoid double calling update_rq_clock() in __balance_push_cpu_stop() — [`96500560_avoid_double_calling_update_rq.md`](bugs/96500560_avoid_double_calling_update_rq.md) <!-- driver:double_clock attempts:1 -->
- [x] `9818427c6270` sched/debug: Make sd->flags sysctl read-only — [`9818427c_make_sd_flags_sysctl_readonly.md`](bugs/9818427c_make_sd_flags_sysctl_readonly.md) <!-- driver:sd_flags_readonly attempts:1 -->
- [-] `9864f5b5943a` cpuidle: Move trace_cpu_idle() into generic code — [`9864f5b5_move_trace_cpu_idle_into.md`](bugs/9864f5b5_move_trace_cpu_idle_into.md) <!-- skipped:tracing-correctness-issue-not-observable-via-kSTEP attempts:1 -->
- [x] `9d0df3779745` Trigger warning if ->migration_disabled counter underflows — [`9d0df377_trigger_warning_if_migration_disabled.md`](bugs/9d0df377_trigger_warning_if_migration_disabled.md) <!-- driver:migration_underflow attempts:1 -->
- [x] `9e81889c7648` sched: Fix affine_move_task() self-concurrency — [`9e81889c_fix_affine_move_task_self.md`](bugs/9e81889c_fix_affine_move_task_self.md) <!-- driver:affine_move_self attempts:1 -->
- [-] `9ed20bafc858` preempt/dynamic: Fix setup_preempt_mode() return value — [`9ed20baf_fix_setup_preempt_mode_return.md`](bugs/9ed20baf_fix_setup_preempt_mode_return.md) <!-- skipped:boot-time-only __setup() return value bug, no runtime scheduling impact attempts:1 -->
- [-] `a06247c6804f` psi: Fix uaf issue when psi trigger is destroyed while being polled — [`a06247c6_fix_uaf_issue_when_psi.md`](bugs/a06247c6_fix_uaf_issue_when_psi.md) <!-- skipped:requires-userspace-file-ops-on-proc-pressure-outside-kstep-scope attempts:1 -->
- [x] `abfc01077df6` sched: Fix do_set_cpus_allowed() locking — [`abfc0107_fix_do_set_cpus_allowed.md`](bugs/abfc0107_fix_do_set_cpus_allowed.md) <!-- driver:set_cpus_allowed_lock attempts:1 -->
- [x] `af13e5e437dc` sched: Fix the do_set_cpus_allowed() locking fix — [`af13e5e4_fix_the_do_set_cpus.md`](bugs/af13e5e4_fix_the_do_set_cpus.md) <!-- driver:do_set_cpus_fix attempts:1 -->
- [x] `b1e8206582f9` sched: Fix yet more sched_fork() races — [`b1e82065_yet_more_sched_fork_races.md`](bugs/b1e82065_yet_more_sched_fork_races.md) <!-- driver:sched_fork_race attempts:1 -->
- [x] `b5c4477366fb` sched: Use cpu_dying() to fix balance_push vs hotplug-rollback — [`b5c44773_use_cpu_dying_to_fix.md`](bugs/b5c44773_use_cpu_dying_to_fix.md) <!-- driver:balance_push_hotplug attempts:1 -->
- [x] `b6e13e85829f` sched/core: Fix ttwu() race — [`b6e13e85_ttwu_race.md`](bugs/b6e13e85_ttwu_race.md) <!-- driver:ttwu_race attempts:1 -->
- [-] `bc1cca97e6da` sched/debug: Show the registers of 'current' in dump_cpu_task() — [`bc1cca97_show_the_registers_of_current.md`](bugs/bc1cca97_show_the_registers_of_current.md) <!-- skipped:diagnostic-output-quality-bug-on-non-NMI-archs-not-reproducible-on-x86 attempts:1 -->
- [-] `bf2c59fce407` sched/core: Fix illegal RCU from offline CPUs — [`bf2c59fc_illegal_rcu_from_offline_cpus.md`](bugs/bf2c59fc_illegal_rcu_from_offline_cpus.md) <!-- skipped:requires-cpu-hotplug-not-supported-by-kstep attempts:1 -->
- [x] `c2ae8b0df2d1` sched/core: Fix psi_dequeue() for Proxy Execution — [`c2ae8b0d_psi_dequeue_for_proxy_execution.md`](bugs/c2ae8b0d_psi_dequeue_for_proxy_execution.md) <!-- driver:psi_dequeue_proxy attempts:1 -->
- [x] `c662e2b1e8cf` sched: Fix sched_delayed vs sched_core — [`c662e2b1_sched_delayed_vs_sched_core.md`](bugs/c662e2b1_sched_delayed_vs_sched_core.md) <!-- driver:delayed_vs_core attempts:1 -->
- [x] `ca4984a7dd86` sched: Fix UCLAMP_FLAG_IDLE setting — [`ca4984a7_uclamp_flag_idle_setting.md`](bugs/ca4984a7_uclamp_flag_idle_setting.md) <!-- driver:uclamp_flag_idle attempts:1 -->
- [x] `ce3614daabea` sched: Fix unreliable rseq cpu_id for new tasks — [`ce3614da_fix_rseq_cpu_id_tasks.md`](bugs/ce3614da_fix_rseq_cpu_id_tasks.md) <!-- driver:rseq_cpu_id_fork attempts:1 -->
- [ ] `d136122f5845` sched: Fix race against ptrace_freeze_trace() — [`d136122f_fix_race_ptrace_freeze.md`](bugs/d136122f_fix_race_ptrace_freeze.md)
- [ ] `d707faa64d03` sched/core: Add missing completion for affine_move_task() waiters — [`d707faa6_affine_move_task_completion.md`](bugs/d707faa6_affine_move_task_completion.md)
- [ ] `dbfb089d360b` sched: Fix loadavg accounting race — [`dbfb089d_fix_loadavg_accounting_race.md`](bugs/dbfb089d_fix_loadavg_accounting_race.md)
- [ ] `dcd6dffb0a75` sched/core: Fix size of rq::uclamp initialization — [`dcd6dffb_fix_rq_uclamp_init_size.md`](bugs/dcd6dffb_fix_rq_uclamp_init_size.md)
- [ ] `df14b7f9efcd` sched/core: Fix a missed update of user_cpus_ptr — [`df14b7f9_core_missed_user_cpus_ptr.md`](bugs/df14b7f9_core_missed_user_cpus_ptr.md)
- [ ] `e0b257c3b71b` sched: Prevent raising SCHED_SOFTIRQ when CPU is !active — [`e0b257c3_prevent_softirq_inactive_cpu.md`](bugs/e0b257c3_prevent_softirq_inactive_cpu.md)
- [ ] `e38e5299747b` sched/hrtick: Fix hrtick() vs. scheduling context — [`e38e5299_hrtick_schedule_context.md`](bugs/e38e5299_hrtick_schedule_context.md)
- [ ] `e65855a52b47` Fix a deadlock when enabling uclamp static key — [`e65855a5_fix_deadlock_enabling_uclamp_static.md`](bugs/e65855a5_fix_deadlock_enabling_uclamp_static.md)
- [ ] `e932c4ab38f0` sched/core: Prevent wakeup of ksoftirqd during idle load balance — [`e932c4ab_prevent_wakeup_ksoftirqd_during_idle.md`](bugs/e932c4ab_prevent_wakeup_ksoftirqd_during_idle.md)
- [ ] `eaf5a92ebde5` sched/core: Fix reset-on-fork from RT with uclamp — [`eaf5a92e_fix_resetonfork_rt_uclamp.md`](bugs/eaf5a92e_fix_resetonfork_rt_uclamp.md)
- [ ] `ec618b84f6e1` sched: Fix rq->nr_iowait ordering — [`ec618b84_fix_rqnr_iowait_ordering.md`](bugs/ec618b84_fix_rqnr_iowait_ordering.md)
- [ ] `ef73d6a4ef0b` sched/wait: Fix a kthread_park race with wait_woken() — [`ef73d6a4_fix_kthread_park_race_wait_woken.md`](bugs/ef73d6a4_fix_kthread_park_race_wait_woken.md)
- [ ] `f1dfdab694eb` sched/vtime: Prevent unstable evaluation of WARN(vtime->state) — [`f1dfdab6_prevent_unstable_evaluation_warnvtimestate.md`](bugs/f1dfdab6_prevent_unstable_evaluation_warnvtimestate.md)
- [ ] `f912d051619d` sched: remove redundant on_rq status change — [`f912d051_remove_redundant_on_rq_status_change.md`](bugs/f912d051_remove_redundant_on_rq_status_change.md)
- [ ] `fd844ba9ae59` Check cpus_mask, not cpus_ptr in __set_cpus_allowed_ptr(), to fix mask corruption — [`fd844ba9_check_cpus_mask_not_cpus_ptr___set_cpus_allowed_ptr.md`](bugs/fd844ba9_check_cpus_mask_not_cpus_ptr___set_cpus_allowed_ptr.md)
- [ ] `fe7a11c78d2a` Fix unbalance set_rq_online/offline() in sched_cpu_deactivate() — [`fe7a11c7_fix_unbalance_set_rq_onlineoffline_sched_cpu_deactivate.md`](bugs/fe7a11c7_fix_unbalance_set_rq_onlineoffline_sched_cpu_deactivate.md)

## CFS (Completely Fair Scheduler) (32)

- [ ] `02dbb7246c5b` Fix clearing of has_idle_cores flag in select_idle_cpu() — [`02dbb724_fix_clearing_of_has_idle.md`](bugs/02dbb724_fix_clearing_of_has_idle.md)
- [ ] `111688ca1c4a` Fix negative imbalance in imbalance calculation — [`111688ca_fix_negative_imbalance_in_imbalance.md`](bugs/111688ca_fix_negative_imbalance_in_imbalance.md)
- [ ] `15257cc2f905` sched/fair: Revert force wakeup preemption — [`15257cc2_revert_force_wakeup_preemption.md`](bugs/15257cc2_revert_force_wakeup_preemption.md)
- [ ] `289de3598481` sched/fair: Fix statistics for find_idlest_group() — [`289de359_statistics_for_find_idlest_group.md`](bugs/289de359_statistics_for_find_idlest_group.md)
- [ ] `2ae891b82695` sched: Reduce the default slice to avoid tasks getting an extra tick — [`2ae891b8_the_default_slice_to_avoid.md`](bugs/2ae891b8_the_default_slice_to_avoid.md)
- [ ] `39a2a6eb5c9b` sched/fair: Fix shift-out-of-bounds in load_balance() — [`39a2a6eb_shift_out_of_bounds_in.md`](bugs/39a2a6eb_shift_out_of_bounds_in.md)
- [ ] `3a0baa8e6c57` sched/fair: Fix entity's lag with run to parity — [`3a0baa8e_entity_s_lag_with_run.md`](bugs/3a0baa8e_entity_s_lag_with_run.md)
- [ ] `5ab297bab984` sched/fair: Fix reordering of enqueue/dequeue_task_fair() — [`5ab297ba_reordering_enqueuedequeue_task_fair.md`](bugs/5ab297ba_reordering_enqueuedequeue_task_fair.md)
- [ ] `6212437f0f60` sched/fair: Fix runnable_avg for throttled cfs — [`6212437f_runnable_avg_throttled_cfs.md`](bugs/6212437f_runnable_avg_throttled_cfs.md)
- [ ] `64eaf50731ac` Fix cfs_rq_clock_pelt() for throttled cfs_rq — [`64eaf507_cfs_rq_clock_pelt_throttled_cfs_rq.md`](bugs/64eaf507_cfs_rq_clock_pelt_throttled_cfs_rq.md)
- [ ] `66951e4860d3` sched/fair: Fix update_cfs_group() vs DELAY_DEQUEUE — [`66951e48_update_cfs_group_delay_dequeue.md`](bugs/66951e48_update_cfs_group_delay_dequeue.md)
- [ ] `68d7a190682a` sched/fair: Fix util_est UTIL_AVG_UNCHANGED handling — [`68d7a190_util_est_util_avg_unchanged_handling.md`](bugs/68d7a190_util_est_util_avg_unchanged_handling.md)
- [ ] `6ab7973f2540` sched/fair: Fix sched_avg fold — [`6ab7973f_sched_avg_fold.md`](bugs/6ab7973f_sched_avg_fold.md)
- [ ] `6c8116c914b6` sched/fair: Fix condition of avg_load calculation — [`6c8116c9_condition_avg_load_calculation.md`](bugs/6c8116c9_condition_avg_load_calculation.md)
- [ ] `6d7e4782bcf5` sched/fair: Fix the decision for load balance — [`6d7e4782_the_decision_for_load_balance.md`](bugs/6d7e4782_the_decision_for_load_balance.md)
- [ ] `729288bc6856` Fix util_est accounting for DELAY_DEQUEUE — [`729288bc_utilest_accounting_for_delaydequeue.md`](bugs/729288bc_utilest_accounting_for_delaydequeue.md)
- [ ] `72bffbf57c52` Fix initial util_avg calculation — [`72bffbf5_initial_utilavg_calculation.md`](bugs/72bffbf5_initial_utilavg_calculation.md)
- [ ] `74eec63661d4` sched/fair: Fix NO_RUN_TO_PARITY case — [`74eec636_noruntoparity_case.md`](bugs/74eec636_noruntoparity_case.md)
- [ ] `8c1f560c1ea3` Avoid stale CPU util_est value for schedutil in task dequeue — [`8c1f560c_avoid_stale_cpu_util_est.md`](bugs/8c1f560c_avoid_stale_cpu_util_est.md)
- [ ] `8e1ac4299a6e` sched/fair: Fix overutilized update in enqueue_task_fair() — [`8e1ac429_fix_overutilized_update_enqueue_task.md`](bugs/8e1ac429_fix_overutilized_update_enqueue_task.md)
- [ ] `9b5ce1a37e90` sched: Fix sched_delayed vs cfs_bandwidth — [`9b5ce1a3_fix_sched_delayed_vs_cfs.md`](bugs/9b5ce1a3_fix_sched_delayed_vs_cfs.md)
- [ ] `a430d99e3490` Fix value reported by hot tasks pulled in /proc/schedstat — [`a430d99e_fix_value_reported_by_hot.md`](bugs/a430d99e_fix_value_reported_by_hot.md)
- [ ] `aa4f74dfd42b` sched: Fix runtime accounting w/ split exec & sched contexts — [`aa4f74df_fix_runtime_accounting_split_exec.md`](bugs/aa4f74df_fix_runtime_accounting_split_exec.md)
- [ ] `b34cb07dde7c` sched/fair: Fix enqueue_task_fair() warning some more — [`b34cb07d_enqueue_task_fair_warning_some.md`](bugs/b34cb07d_enqueue_task_fair_warning_some.md)
- [ ] `b3d99f43c72b` sched/fair: Fix zero_vruntime tracking — [`b3d99f43_zero_vruntime_tracking.md`](bugs/b3d99f43_zero_vruntime_tracking.md)
- [ ] `c82a69629c53` sched/fair: fix case with reduced capacity CPU — [`c82a6962_case_with_reduced_capacity_cpu.md`](bugs/c82a6962_case_with_reduced_capacity_cpu.md)
- [ ] `ca125231dd29` Fix unfairness caused by stalled tg_load_avg_contrib when the last task migrates out — [`ca125231_unfairness_caused_by_stalled_tg.md`](bugs/ca125231_unfairness_caused_by_stalled_tg.md)
- [ ] `d206fbad9328` sched/fair: Revert max_newidle_lb_cost bump — [`d206fbad_revert_max_newidle_lb_cost.md`](bugs/d206fbad_revert_max_newidle_lb_cost.md)
- [ ] `e98fa02c4f2e` sched/fair: Eliminate bandwidth race between throttling and distribution — [`e98fa02c_eliminate_bandwidth_race_between_throttling.md`](bugs/e98fa02c_eliminate_bandwidth_race_between_throttling.md)
- [ ] `ebb83d84e49b` Avoid multiple calling update_rq_clock() in __cfsb_csd_unthrottle() — [`ebb83d84_avoid_multiple_calling_update_rq_clock___cfsb_csd_unthrottle.md`](bugs/ebb83d84_avoid_multiple_calling_update_rq_clock___cfsb_csd_unthrottle.md)
- [ ] `f60a631ab9ed` Fix tg->load when offlining a CPU — [`f60a631a_fix_tgload_offlining_cpu.md`](bugs/f60a631a_fix_tgload_offlining_cpu.md)
- [ ] `fe61468b2cbc` sched/fair: Fix enqueue_task_fair warning — [`fe61468b_fix_enqueue_task_fair_warning.md`](bugs/fe61468b_fix_enqueue_task_fair_warning.md)

## Deadline (24)

- [ ] `115135422562` sched/deadline: Fix 'stuck' dl_server — [`11513542_fix_stuck_dl_server.md`](bugs/11513542_fix_stuck_dl_server.md)
- [ ] `22368fe1f9bb` sched/deadline: Fix replenish_dl_new_period dl_server condition — [`22368fe1_replenish_dl_new_period_dl.md`](bugs/22368fe1_replenish_dl_new_period_dl.md)
- [ ] `2279f540ea7d` sched/deadline: Fix priority inheritance with multiple scheduling classes — [`2279f540_priority_inheritance_with_multiple_scheduling.md`](bugs/2279f540_priority_inheritance_with_multiple_scheduling.md)
- [ ] `421fc59cf58c` sched/deadline: Fix RT task potential starvation when expiry time passed — [`421fc59c_fix_rt_task_potential_starvation.md`](bugs/421fc59c_fix_rt_task_potential_starvation.md)
- [ ] `46fcc4b00c3c` sched/deadline: Fix stale throttling on de-/boosted tasks — [`46fcc4b0_fix_stale_throttling_on_de.md`](bugs/46fcc4b0_fix_stale_throttling_on_de.md)
- [ ] `4717432dfd99` sched/deadline: Fix dl_server_stopped() — [`4717432d_fix_dl_server_stopped.md`](bugs/4717432d_fix_dl_server_stopped.md)
- [ ] `4ae8d9aa9f9d` sched/deadline: Fix dl_server getting stuck — [`4ae8d9aa_fix_dl_server_getting_stuck.md`](bugs/4ae8d9aa_fix_dl_server_getting_stuck.md)
- [ ] `4de9ff76067b` sched/deadline: Avoid double update_rq_clock() — [`4de9ff76_avoid_double_update_rq_clock.md`](bugs/4de9ff76_avoid_double_update_rq_clock.md)
- [ ] `53916d5fd3c0` sched/deadline: Check bandwidth overflow earlier for hotplug — [`53916d5f_check_bandwidth_overflow_earlier_for.md`](bugs/53916d5f_check_bandwidth_overflow_earlier_for.md)
- [ ] `6a9d623aad89` sched/deadline: Fix bandwidth reclaim equation in GRUB — [`6a9d623a_bandwidth_reclaim_equation_grub.md`](bugs/6a9d623a_bandwidth_reclaim_equation_grub.md)
- [ ] `85989106feb7` sched/deadline: Create DL BW alloc, free & check overflow interface — [`85989106_create_dl_bw_alloc_free.md`](bugs/85989106_create_dl_bw_alloc_free.md)
- [ ] `8fd5485fb4f3` sched/deadline: Fix race in push_dl_task() — [`8fd5485f_fix_race_push_dl_task.md`](bugs/8fd5485f_fix_race_push_dl_task.md)
- [ ] `9c602adb799e` sched/deadline: Fix schedstats vs deadline servers — [`9c602adb_fix_schedstats_vs_deadline_servers.md`](bugs/9c602adb_fix_schedstats_vs_deadline_servers.md)
- [ ] `a3a70caf7906` Fix dl_server behaviour — [`a3a70caf_fix_dl_server_behaviour.md`](bugs/a3a70caf_fix_dl_server_behaviour.md)
- [ ] `b4da13aa28d4` sched/deadline: Fix missing clock update in migrate_task_rq_dl() — [`b4da13aa_missing_clock_update_in_migrate.md`](bugs/b4da13aa_missing_clock_update_in_migrate.md)
- [ ] `b53127db1dbf` sched/dlserver: Fix dlserver double enqueue — [`b53127db_dlserver_double_enqueue.md`](bugs/b53127db_dlserver_double_enqueue.md)
- [ ] `b58652db66c9` sched/deadline: Fix task_struct reference leak — [`b58652db_task_struct_reference_leak.md`](bugs/b58652db_task_struct_reference_leak.md)
- [ ] `bb4700adc3ab` sched/deadline: Always stop dl-server before changing parameters — [`bb4700ad_always_stop_dl_server_before.md`](bugs/bb4700ad_always_stop_dl_server_before.md)
- [ ] `ca1e8eede4fc` sched/deadline: Fix server stopping with runnable tasks — [`ca1e8eed_server_stopping_with_runnable_tasks.md`](bugs/ca1e8eed_server_stopping_with_runnable_tasks.md)
- [ ] `d7d607096ae6` sched/rt: Fix Deadline utilization tracking during policy change — [`d7d60709_rt_deadline_util_policy_change.md`](bugs/d7d60709_rt_deadline_util_policy_change.md)
- [ ] `ddfc710395cc` sched/deadline: Fix BUG_ON condition for deboosted tasks — [`ddfc7103_deadline_deboosted_tasks_bug.md`](bugs/ddfc7103_deadline_deboosted_tasks_bug.md)
- [ ] `f5a538c07df2` sched/deadline: Fix dl_server stop condition — [`f5a538c0_fix_dl_server_stop_condition.md`](bugs/f5a538c0_fix_dl_server_stop_condition.md)
- [ ] `f95091536f78` sched/deadline: Fix reset_on_fork reporting of DL tasks — [`f9509153_fix_reset_on_fork_reporting_dl_tasks.md`](bugs/f9509153_fix_reset_on_fork_reporting_dl_tasks.md)
- [ ] `fc975cfb3639` sched/deadline: Fix dl_server runtime calculation formula — [`fc975cfb_fix_dl_server_runtime_calculation_formula.md`](bugs/fc975cfb_fix_dl_server_runtime_calculation_formula.md)

## CFS (21)

- [ ] `0258bdfaff5b` Fix unfairness caused by missing load decay — [`0258bdfa_fix_unfairness_caused_by_missing.md`](bugs/0258bdfa_fix_unfairness_caused_by_missing.md)
- [ ] `26a8b12747c9` sched/fair: Fix race between runtime distribution and assignment — [`26a8b127_race_between_runtime_distribution_and.md`](bugs/26a8b127_race_between_runtime_distribution_and.md)
- [ ] `26cf52229efc` sched: Avoid scale real weight down to zero — [`26cf5222_scale_real_weight_down_to.md`](bugs/26cf5222_scale_real_weight_down_to.md)
- [ ] `2f8c62296b6f` sched/fair: Fix warning in bandwidth distribution — [`2f8c6229_warning_in_bandwidth_distribution.md`](bugs/2f8c6229_warning_in_bandwidth_distribution.md)
- [ ] `39afe5d6fc59` sched/fair: Fix inaccurate tally of ttwu_move_affine — [`39afe5d6_inaccurate_tally_of_ttwu_move.md`](bugs/39afe5d6_inaccurate_tally_of_ttwu_move.md)
- [ ] `39f23ce07b93` sched/fair: Fix unthrottle_cfs_rq() for leaf_cfs_rq list — [`39f23ce0_unthrottle_cfs_rq_for_leaf.md`](bugs/39f23ce0_unthrottle_cfs_rq_for_leaf.md)
- [ ] `3b4035ddbfc8` sched/fair: Fix potential memory corruption in child_cfs_rq_on_list — [`3b4035dd_fix_potential_memory_corruption_in.md`](bugs/3b4035dd_fix_potential_memory_corruption_in.md)
- [ ] `493afbd187c4` sched/fair: Fix NEXT_BUDDY — [`493afbd1_fix_next_buddy.md`](bugs/493afbd1_fix_next_buddy.md)
- [ ] `52262ee567ad` sched/fair: Allow a per-CPU kthread waking a task to stack on the same CPU, to fix XFS performance regression — [`52262ee5_allow_a_per_cpu_kthread.md`](bugs/52262ee5_allow_a_per_cpu_kthread.md)
- [ ] `6e3c0a4e1ad1` sched/fair: Fix lag clamp — [`6e3c0a4e_lag_clamp.md`](bugs/6e3c0a4e_lag_clamp.md)
- [ ] `72d0ad7cb5ba` sched/fair: Fix CFS bandwidth hrtimer expiry type — [`72d0ad7c_cfs_bandwidth_hrtimer_expiry_type.md`](bugs/72d0ad7c_cfs_bandwidth_hrtimer_expiry_type.md)
- [ ] `7e2edaf61814` Fix another detach on unattached task corner case — [`7e2edaf6_another_detach_on_unattached_task.md`](bugs/7e2edaf6_another_detach_on_unattached_task.md)
- [ ] `82e9d0456e06` sched/fair: Avoid re-setting virtual deadline on 'migrations' — [`82e9d045_avoid_re_setting_virtual_deadline.md`](bugs/82e9d045_avoid_re_setting_virtual_deadline.md)
- [ ] `91dcf1e8068e` sched/fair: Fix imbalance overflow — [`91dcf1e8_fix_imbalance_overflow.md`](bugs/91dcf1e8_fix_imbalance_overflow.md)
- [ ] `956dfda6a708` sched/fair: Prevent cfs_rq from being unthrottled with zero runtime_remaining — [`956dfda6_prevent_cfs_rq_from_being.md`](bugs/956dfda6_prevent_cfs_rq_from_being.md)
- [ ] `aa3ee4f0b754` sched/fair: Fixup wake_up_sync() vs DELAYED_DEQUEUE — [`aa3ee4f0_fixup_wake_up_sync_vs.md`](bugs/aa3ee4f0_fixup_wake_up_sync_vs.md)
- [ ] `af98d8a36a96` sched/fair: Fix CPU bandwidth limit bypass during CPU hotplug — [`af98d8a3_fix_cpu_bandwidth_limit_bypass.md`](bugs/af98d8a3_fix_cpu_bandwidth_limit_bypass.md)
- [ ] `b55945c500c5` sched: Fix pick_next_task_fair() vs try_to_wake_up() race — [`b55945c5_pick_next_task_fair_vs.md`](bugs/b55945c5_pick_next_task_fair_vs.md)
- [ ] `c0490bc9bb62` sched/fair: Fix cfs_rq_is_decayed() on !SMP — [`c0490bc9_cfs_rq_is_decayed_on.md`](bugs/c0490bc9_cfs_rq_is_decayed_on.md)
- [ ] `d4ac164bde7a` sched/eevdf: Fix wakeup-preempt by checking cfs_rq->nr_running — [`d4ac164b_eevdf_wakeup_preempt_nr_running.md`](bugs/d4ac164b_eevdf_wakeup_preempt_nr_running.md)
- [ ] `e21cf43406a1` sched/cfs: change initial value of runnable_avg — [`e21cf434_cfs_runnable_avg_initial.md`](bugs/e21cf434_cfs_runnable_avg_initial.md)

## PSI (Pressure Stall Information) (14)

- [ ] `2fcd7bbae90a` sched/psi: Fix avgs_work re-arm in psi_avgs_work() — [`2fcd7bba_avgs_work_re_arm_in.md`](bugs/2fcd7bba_avgs_work_re_arm_in.md)
- [ ] `3840cbe24cf0` sched: psi: fix bogus pressure spikes from aggregation race — [`3840cbe2_psi_fix_bogus_pressure_spikes.md`](bugs/3840cbe2_psi_fix_bogus_pressure_spikes.md)
- [ ] `570c8efd5eb7` sched/psi: Optimize psi_group_change() cpu_clock() usage — [`570c8efd_sched_psi_optimize_psi_group.md`](bugs/570c8efd_sched_psi_optimize_psi_group.md)
- [ ] `6fcca0fa4811` sched/psi: Fix OOB write when writing 0 bytes to PSI files — [`6fcca0fa_oob_write_when_writing_0.md`](bugs/6fcca0fa_oob_write_when_writing_0.md)
- [ ] `8f91efd870ea` psi: Fix race between psi_trigger_create/destroy — [`8f91efd8_fix_race_between_psi_trigger.md`](bugs/8f91efd8_fix_race_between_psi_trigger.md)
- [ ] `915a087e4c47` psi: Fix trigger being fired unexpectedly at initial — [`915a087e_fix_trigger_being_fired_unexpectedly.md`](bugs/915a087e_fix_trigger_being_fired_unexpectedly.md)
- [ ] `99b773d720ae` sched/psi: Fix psi_seq initialization — [`99b773d7_fix_psi_seq_initialization.md`](bugs/99b773d7_fix_psi_seq_initialization.md)
- [ ] `9d10a13d1e4c` sched,psi: Handle potential task count underflow bugs more gracefully — [`9d10a13d_handle_potential_task_count_underflow.md`](bugs/9d10a13d_handle_potential_task_count_underflow.md)
- [ ] `c2dbe32d5db5` sched/psi: Fix use-after-free in ep_remove_wait_queue() — [`c2dbe32d_use_after_free_in_ep.md`](bugs/c2dbe32d_use_after_free_in_ep.md)
- [ ] `c530a3c716b9` sched/psi: Fix periodic aggregation shut off — [`c530a3c7_periodic_aggregation_shut_off.md`](bugs/c530a3c7_periodic_aggregation_shut_off.md)
- [ ] `cb0e52b77487` psi: Fix PSI_MEM_FULL state when tasks are in memstall and doing reclaim — [`cb0e52b7_psi_mem_full_state_when.md`](bugs/cb0e52b7_psi_mem_full_state_when.md)
- [ ] `d583d360a620` psi: Fix psi state corruption when schedule() races with cgroup move — [`d583d360_psi_state_corruption_cgroup_move.md`](bugs/d583d360_psi_state_corruption_cgroup_move.md)
- [ ] `e38f89af6a13` sched/psi: Fix possible missing or delayed pending event — [`e38f89af_psi_missing_pending_event.md`](bugs/e38f89af_psi_missing_pending_event.md)
- [ ] `e6df4ead85d9` psi: fix possible trigger missing in the window — [`e6df4ead_fix_possible_trigger_missing_window.md`](bugs/e6df4ead_fix_possible_trigger_missing_window.md)

## topology (11)

- [ ] `01bb11ad828b` sched/topology: fix KASAN warning in hop_cmp() — [`01bb11ad_fix_kasan_warning_in_hop.md`](bugs/01bb11ad_fix_kasan_warning_in_hop.md)
- [ ] `0fb3978b0aac` sched/numa: Fix NUMA topology for systems with CPU-less nodes — [`0fb3978b_fix_numa_topology_for_systems.md`](bugs/0fb3978b_fix_numa_topology_for_systems.md)
- [ ] `585b6d2723dc` sched/topology: fix the issue groups don't span domain->span for NUMA diameter > 2 — [`585b6d27_fix_the_issue_groups_dont.md`](bugs/585b6d27_fix_the_issue_groups_dont.md)
- [ ] `5ebf512f3350` sched: Fix sched_numa_find_nth_cpu() if mask offline — [`5ebf512f_sched_numa_find_nth_cpu_mask_offline.md`](bugs/5ebf512f_sched_numa_find_nth_cpu_mask_offline.md)
- [ ] `617f2c38cb5c` sched/topology: Fix sched_numa_find_nth_cpu() in CPU-less case — [`617f2c38_sched_numa_find_nth_cpu_cpuless_case.md`](bugs/617f2c38_sched_numa_find_nth_cpu_cpuless_case.md)
- [ ] `61bb6cd2f765` mm: move node_reclaim_distance to fix NUMA without SMP — [`61bb6cd2_move_node_reclaim_distance_fix_numa_without.md`](bugs/61bb6cd2_move_node_reclaim_distance_fix_numa_without.md)
- [ ] `71e5f6644fb2` sched/topology: Fix sched_domain_topology_level alloc in sched_init_numa() — [`71e5f664_scheddomaintopologylevel_alloc_in_schedinitnuma.md`](bugs/71e5f664_scheddomaintopologylevel_alloc_in_schedinitnuma.md)
- [ ] `7f434dff7621` sched/topology: Remove redundant variable and fix incorrect type in build_sched_domains — [`7f434dff_remove_redundant_variable_and_fix.md`](bugs/7f434dff_remove_redundant_variable_and_fix.md)
- [ ] `848785df4883` sched/topology: Move sd_flag_debug out of #ifdef CONFIG_SYSCTL — [`848785df_move_sd_flag_debug_out.md`](bugs/848785df_move_sd_flag_debug_out.md)
- [ ] `8f833c82cdab` sched/topology: Change behaviour of the 'sched_energy_aware' sysctl, based on the platform — [`8f833c82_change_behaviour_sched_energy_aware.md`](bugs/8f833c82_change_behaviour_sched_energy_aware.md)
- [ ] `8fca9494d4b4` sched/topology: Move sd_flag_debug out of linux/sched/topology.h — [`8fca9494_move_sd_flag_debug_out_1.md`](bugs/8fca9494_move_sd_flag_debug_out_1.md)

## RT (5)

- [ ] `079be8fc6309` sched/rt: Disallow writing invalid values to sched_rt_period_us — [`079be8fc_disallow_writing_invalid_values_to.md`](bugs/079be8fc_disallow_writing_invalid_values_to.md)
- [ ] `28f152cd0926` sched/rt: fix build error when CONFIG_SYSCTL is disable — [`28f152cd_build_error_when_config_sysctl.md`](bugs/28f152cd_build_error_when_config_sysctl.md)
- [ ] `87514b2c24f2` Fix Sparse warnings due to undefined rt.c declarations — [`87514b2c_fix_sparse_warnings_due_undefined.md`](bugs/87514b2c_fix_sparse_warnings_due_undefined.md)
- [ ] `c7fcb99877f9` Fix sysctl_sched_rr_timeslice intial value — [`c7fcb998_sysctl_sched_rr_timeslice_intial.md`](bugs/c7fcb998_sysctl_sched_rr_timeslice_intial.md)
- [ ] `fc09027786c9` sched/rt: Fix live lock between select_fallback_rq() and RT push — [`fc090277_fix_live_lock_between_select_fallback_rq.md`](bugs/fc090277_fix_live_lock_between_select_fallback_rq.md)

## cpufreq (3)

- [ ] `0070ea296239` cpufreq: schedutil: restore cached freq when next_f is not changed — [`0070ea29_schedutil_restore_cached_freq_when.md`](bugs/0070ea29_schedutil_restore_cached_freq_when.md)
- [ ] `79443a7e9da3` cpufreq/sched: Explicitly synchronize limits_changed flag handling — [`79443a7e_explicitly_synchronize_limitschanged_flag_handling.md`](bugs/79443a7e_explicitly_synchronize_limitschanged_flag_handling.md)
- [ ] `e37617c8e53a` sched/fair: Fix frequency selection for non-invariant case — [`e37617c8_fair_freq_non_invariant.md`](bugs/e37617c8_fair_freq_non_invariant.md)

## CFS (Fair scheduling) (3)

- [ ] `014ba44e8184` Fix per-CPU kthread and wakee stacking for asym CPU capacity — [`014ba44e_fix_per_cpu_kthread_and.md`](bugs/014ba44e_fix_per_cpu_kthread_and.md)
- [ ] `17e3e88ed0b6` Fix pelt lost idle time detection — [`17e3e88e_fix_pelt_lost_idle_time.md`](bugs/17e3e88e_fix_pelt_lost_idle_time.md)
- [ ] `df3cb4ea1fb6` sched/fair: Fix wrong cpu selecting from isolated domain — [`df3cb4ea_fair_cpu_isolated_domain.md`](bugs/df3cb4ea_fair_cpu_isolated_domain.md)

## CFS (fair scheduling) (3)

- [ ] `2a4b03ffc69f` sched/fair: Prevent unlimited runtime on throttled group — [`2a4b03ff_unlimited_runtime_on_throttled_group.md`](bugs/2a4b03ff_unlimited_runtime_on_throttled_group.md)
- [ ] `8b4e74ccb582` sched/fair: Fix detection of per-CPU kthreads waking a task — [`8b4e74cc_fix_detection_percpu_kthreads_waking.md`](bugs/8b4e74cc_fix_detection_percpu_kthreads_waking.md)
- [ ] `c1f43c342e1f` sched/fair: Fix sched_can_stop_tick() for fair tasks — [`c1f43c34_sched_can_stop_tick_for.md`](bugs/c1f43c34_sched_can_stop_tick_for.md)

## RT (Real-Time scheduler) (3)

- [ ] `5c66d1b9b30f` nohz/full, sched/rt: Fix missed tick-reenabling bug in dequeue_task_rt() — [`5c66d1b9_schedrt_fix_missed_tickreenabling_bug.md`](bugs/5c66d1b9_schedrt_fix_missed_tickreenabling_bug.md)
- [ ] `f558c2b834ec` sched/rt: Fix double enqueue caused by rt_effective_prio — [`f558c2b8_fix_double_enqueue_caused_rt_effective_prio.md`](bugs/f558c2b8_fix_double_enqueue_caused_rt_effective_prio.md)
- [ ] `fecfcbc288e9` sched/rt: Fix RT utilization tracking during policy change — [`fecfcbc2_fix_rt_utilization_tracking_during.md`](bugs/fecfcbc2_fix_rt_utilization_tracking_during.md)

## Deadline (SCHED_DEADLINE) (3)

- [ ] `5e42d4c123ba` sched/deadline: Prepare for switched_from() change — [`5e42d4c1_prepare_switched_from_change.md`](bugs/5e42d4c1_prepare_switched_from_change.md)
- [ ] `627cc25f8446` sched/deadline: Use ENQUEUE_MOVE to allow priority change — [`627cc25f_use_enqueue_move_allow_priority_change.md`](bugs/627cc25f_use_enqueue_move_allow_priority_change.md)
- [ ] `a57415f5d1e4` Fix sched_dl_global_validate() — [`a57415f5_fix_sched_dl_global_validate.md`](bugs/a57415f5_fix_sched_dl_global_validate.md)

## NUMA (3)

- [ ] `9709eb0f845b` sched/numa: fix task swap by skipping kernel threads — [`9709eb0f_fix_task_swap_skipping_kernel.md`](bugs/9709eb0f_fix_task_swap_skipping_kernel.md)
- [ ] `9c70b2a33cd2` sched/numa: Fix the potential null pointer dereference in task_numa_work() — [`9c70b2a3_fix_the_potential_null_pointer.md`](bugs/9c70b2a3_fix_the_potential_null_pointer.md)
- [ ] `ab31c7fd2d37` sched/numa: Fix boot crash on arm64 systems — [`ab31c7fd_fix_boot_crash_on_arm64.md`](bugs/ab31c7fd_fix_boot_crash_on_arm64.md)

## RT (Real-Time Scheduling) (2)

- [ ] `089768dfeb3a` sched/rt: Change the type of 'sysctl_sched_rt_period' from 'unsigned int' to 'int' — [`089768df_change_the_type_of_sysctl.md`](bugs/089768df_change_the_type_of_sysctl.md)
- [ ] `690e47d1403e` Fix race in push_rt_task — [`690e47d1_race_push_rt_task.md`](bugs/690e47d1_race_push_rt_task.md)

## core (uclamp) (2)

- [ ] `0c18f2ecfcc2` sched/uclamp: Fix wrong implementation of cpu.uclamp.min — [`0c18f2ec_fix_wrong_implementation_of_cpu.md`](bugs/0c18f2ec_fix_wrong_implementation_of_cpu.md)
- [ ] `315c4f884800` sched/uclamp: Fix rq->uclamp_max not set on first enqueue — [`315c4f88_rq_uclamp_max_not_set.md`](bugs/315c4f88_rq_uclamp_max_not_set.md)

## CFS/EEVDF (2)

- [ ] `1560d1f6eb6b` sched/eevdf: Prevent vlag from going out of bounds in reweight_eevdf() — [`1560d1f6_prevent_vlag_from_going_out.md`](bugs/1560d1f6_prevent_vlag_from_going_out.md)
- [ ] `bbce3de72be5` sched/eevdf: Fix se->slice being set to U64_MAX and resulting crash — [`bbce3de7_se_slice_being_set_to.md`](bugs/bbce3de7_se_slice_being_set_to.md)

## CFS (Fair scheduler) (2)

- [ ] `2feab2492deb` Revert "sched/fair: Make sure to try to detach at least one movable task" — [`2feab249_sched_fair_make_sure_to.md`](bugs/2feab249_sched_fair_make_sure_to.md)
- [ ] `cd9626e9ebc7` sched/fair: Fix external p->on_rq users — [`cd9626e9_external_p_on_rq_users.md`](bugs/cd9626e9_external_p_on_rq_users.md)

## core scheduling (2)

- [ ] `3c474b3239f1` sched: Fix Core-wide rq->lock for uninitialized CPUs — [`3c474b32_fix_core_wide_rq_lock.md`](bugs/3c474b32_fix_core_wide_rq_lock.md)
- [ ] `97886d9dcd86` sched: Migration changes for core scheduling — [`97886d9d_migration_changes_for_core_scheduling.md`](bugs/97886d9d_migration_changes_for_core_scheduling.md)

## Deadline (DL server) (2)

- [ ] `5a40a9bb56d4` sched/debug: Fix dl_server (re)start conditions — [`5a40a9bb_dl_server_restart_conditions.md`](bugs/5a40a9bb_dl_server_restart_conditions.md)
- [ ] `68ec89d0e991` sched/debug: Stop and start server based on if it was active — [`68ec89d0_stop_start_server_based_it.md`](bugs/68ec89d0_stop_start_server_based_it.md)

## CFS (Fair Scheduling) (2)

- [ ] `75b6499024a6` sched/fair: Properly deactivate sched_delayed task upon class change — [`75b64990_properly_deactivate_scheddelayed_task_upon.md`](bugs/75b64990_properly_deactivate_scheddelayed_task_upon.md)
- [ ] `df16b71c686c` sched/fair: Allow changing cgroup of new forked task — [`df16b71c_fair_cgroup_change_forked_task.md`](bugs/df16b71c_fair_cgroup_change_forked_task.md)

## core (isolation/housekeeping) (2)

- [ ] `b7eb4edcc3b5` sched/isolation: Flush memcg workqueues on cpuset isolated partition change — [`b7eb4edc_flush_memcg_workqueues_on_cpuset.md`](bugs/b7eb4edc_flush_memcg_workqueues_on_cpuset.md)
- [ ] `ce84ad5e994a` sched/isolation: Flush vmstat workqueues on cpuset isolated partition change — [`ce84ad5e_isolation_flush_vmstat_cpuset.md`](bugs/ce84ad5e_isolation_flush_vmstat_cpuset.md)

## cpuacct (2)

- [ ] `dbe9337109c2` sched/cpuacct: Fix charge cpuacct.usage_sys — [`dbe93371_cpuacct_charge_usage_sys.md`](bugs/dbe93371_cpuacct_charge_usage_sys.md)
- [ ] `dd02d4234c9a` sched/cpuacct: Fix user/system in shown cpuacct.usage* — [`dd02d423_cpuacct_user_system_usage.md`](bugs/dd02d423_cpuacct_user_system_usage.md)

## sched/debug (1)

- [ ] `02d4ac5885a1` sched/debug: Reset watchdog on all CPUs while processing sysrq-t — [`02d4ac58_reset_watchdog_on_all_cpus.md`](bugs/02d4ac58_reset_watchdog_on_all_cpus.md)

## CFS (Fair Scheduler), Energy-Aware Scheduling (1)

- [ ] `0372e1cf70c2` sched/fair: Fix task utilization accountability in compute_energy() — [`0372e1cf_fix_task_utilization_accountability_in.md`](bugs/0372e1cf_fix_task_utilization_accountability_in.md)

## Deadline (real-time scheduler) (1)

- [ ] `0664e2c311b9` sched/deadline: Fix warning in migrate_enable for boosted tasks — [`0664e2c3_fix_warning_in_migrate_enable.md`](bugs/0664e2c3_fix_warning_in_migrate_enable.md)

## core (MM CID) (1)

- [ ] `0d032a43ebeb` sched/mmcid: Prevent pointless work in mm_update_cpus_allowed() — [`0d032a43_prevent_pointless_work_in_mm.md`](bugs/0d032a43_prevent_pointless_work_in_mm.md)

## core, RT, Deadline (1)

- [ ] `120455c514f7` sched: Fix hotplug vs CPU bandwidth control — [`120455c5_fix_hotplug_vs_cpu_bandwidth.md`](bugs/120455c5_fix_hotplug_vs_cpu_bandwidth.md)

## fair (CFS) (1)

- [ ] `13765de8148f` sched/fair: Fix fault in reweight_entity — [`13765de8_fix_fault_in_reweight_entity.md`](bugs/13765de8_fix_fault_in_reweight_entity.md)

## core scheduler (nohz idle load balancing) (1)

- [ ] `19a1f5ec6999` sched: Fix smp_call_function_single_async() usage for ILB — [`19a1f5ec_fix_smp_call_function_single.md`](bugs/19a1f5ec_fix_smp_call_function_single.md)

## NUMA, SMT (Simultaneous Multi-Threading) (1)

- [ ] `1c6829cfd3d5` sched/numa: Fix is_core_idle() — [`1c6829cf_fix_is_core_idle.md`](bugs/1c6829cf_fix_is_core_idle.md)

## core (CPU hotplug, lazy TLB) (1)

- [ ] `21641bd9a7a7` lazy tlb: fix hotplug exit race with MMU_LAZY_TLB_SHOOTDOWN — [`21641bd9_hotplug_exit_race_with_mmu.md`](bugs/21641bd9_hotplug_exit_race_with_mmu.md)

## CFS, core scheduling (1)

- [ ] `22dc02f81cdd` Revert "sched/fair: Move unused stub functions to header" — [`22dc02f8_sched_fair_move_unused_stub.md`](bugs/22dc02f8_sched_fair_move_unused_stub.md)

## CFS, EAS (Energy-Aware Scheduling), uclamp (1)

- [ ] `244226035a1f` Fix fits_capacity() check in feec() — [`24422603_fits_capacity_check_in_feec.md`](bugs/24422603_fits_capacity_check_in_feec.md)

## core (housekeeping/isolation) (1)

- [ ] `257bf89d8412` sched/isolation: Fix boot crash when maxcpus < first housekeeping CPU — [`257bf89d_boot_crash_when_maxcpus_first.md`](bugs/257bf89d_boot_crash_when_maxcpus_first.md)

## RT scheduler, Deadline scheduler, core scheduler (1)

- [ ] `2679a83731d5` sched/core: Avoid obvious double update_rq_clock warning — [`2679a837_obvious_double_update_rq_clock.md`](bugs/2679a837_obvious_double_update_rq_clock.md)

## sched/mm_cid (1)

- [ ] `2bdf777410dc` sched/mm_cid: Prevent NULL mm dereference in sched_mm_cid_after_execve() — [`2bdf7774_sched_mm_cid_prevent_null.md`](bugs/2bdf7774_sched_mm_cid_prevent_null.md)

## NUMA balancing / Memory tiering (1)

- [ ] `337135e6124b` mm: memory-tiering: fix PGPROMOTE_CANDIDATE counting — [`337135e6_mm_memory_tiering_fix_pgpromote.md`](bugs/337135e6_mm_memory_tiering_fix_pgpromote.md)

## CFS (Fair Scheduler) (1)

- [ ] `3429dd57f0de` Fix inaccurate h_nr_runnable accounting with delayed dequeue — [`3429dd57_inaccurate_h_nr_runnable_accounting.md`](bugs/3429dd57_inaccurate_h_nr_runnable_accounting.md)

## Core scheduler (loadavg) (1)

- [ ] `36569780b0d6` sched: Change nr_uninterruptible type to unsigned long — [`36569780_change_nr_uninterruptible_type_to.md`](bugs/36569780_change_nr_uninterruptible_type_to.md)

## sched_ext (1)

- [ ] `37477d9ecabd` sched_ext: idle: Fix return code of scx_select_cpu_dfl() — [`37477d9e_idle_fix_return_code_of.md`](bugs/37477d9e_idle_fix_return_code_of.md)

## NOHZ load balancing (Fair scheduling) (1)

- [ ] `3ea2f097b17e` Fix NOHZ next idle balance — [`3ea2f097_fix_nohz_next_idle_balance.md`](bugs/3ea2f097_fix_nohz_next_idle_balance.md)

## PELT (Per-Entity Load Tracking) (1)

- [ ] `40f5aa4c5eae` sched/pelt: Fix attach_entity_load_avg() corner case — [`40f5aa4c_sched_pelt_fix_attach_entity.md`](bugs/40f5aa4c_sched_pelt_fix_attach_entity.md)

## Deadline scheduler, topology/root domain management (1)

- [ ] `41d4200b7103` sched/deadline: Restore dl_server bandwidth on non-destructive root domain changes — [`41d4200b_restore_dl_server_bandwidth_on.md`](bugs/41d4200b_restore_dl_server_bandwidth_on.md)

## core (MM_CID management) (1)

- [ ] `4327fb13fa47` sched/mmcid: Prevent live lock on task to CPU mode transition — [`4327fb13_prevent_live_lock_on_task.md`](bugs/4327fb13_prevent_live_lock_on_task.md)

## Deadline, RT (1)

- [ ] `440989c10f4e` sched/deadline: Fix accounting after global limits change — [`440989c1_fix_accounting_after_global_limits.md`](bugs/440989c1_fix_accounting_after_global_limits.md)

## CFS (Completely Fair Scheduler) / Load Balancing (1)

- [ ] `450e749707bc` sched/fair: Fix SMT4 group_smt_balance handling — [`450e7497_fix_smt4_group_smt_balance.md`](bugs/450e7497_fix_smt4_group_smt_balance.md)

## CFS, uclamp (1)

- [ ] `48d5e9daa8b7` sched/uclamp: Fix relationship between uclamp and migration margin — [`48d5e9da_fix_relationship_between_uclamp_and.md`](bugs/48d5e9da_fix_relationship_between_uclamp_and.md)

## core (CFS bandwidth) (1)

- [ ] `49217ea147df` Fix incorrect initialization of the 'burst' parameter in cpu_max_write() — [`49217ea1_fix_incorrect_initialization_of_the.md`](bugs/49217ea1_fix_incorrect_initialization_of_the.md)

## RT (Real-Time) (1)

- [ ] `49bef33e4b87` sched/rt: Plug rt_mutex_setprio() vs push_rt_task() race — [`49bef33e_plug_rt_mutex_setprio_vs.md`](bugs/49bef33e_plug_rt_mutex_setprio_vs.md)

## CFS (Fair) (1)

- [ ] `4ae0c2b91110` sched/debug: Fix fair_server_period_max value — [`4ae0c2b9_fix_fair_server_period_max.md`](bugs/4ae0c2b9_fix_fair_server_period_max.md)

## core scheduling, deadline server (1)

- [ ] `4b26cfdd3956` sched/core: Fix priority checking for DL server picks — [`4b26cfdd_fix_priority_checking_for_dl.md`](bugs/4b26cfdd_fix_priority_checking_for_dl.md)

## CFS, topology (1)

- [ ] `4c58f57fa6e9` Fix sgc->{min,max}_capacity calculation for SD_OVERLAP — [`4c58f57f_fix_sgc_minmax_capacity_calculation.md`](bugs/4c58f57f_fix_sgc_minmax_capacity_calculation.md)

## core scheduler (1)

- [ ] `4ef0c5c6b5ba` kernel/sched: Fix sched_fork() access an invalid sched_task_group — [`4ef0c5c6_kernel_sched_fix_sched_fork.md`](bugs/4ef0c5c6_kernel_sched_fix_sched_fork.md)

## PELT (Per-Entity Load Tracking), CFS (1)

- [ ] `50181c0cff31` sched/pelt: Avoid underestimation of task utilization — [`50181c0c_sched_pelt_avoid_underestimation_of.md`](bugs/50181c0c_sched_pelt_avoid_underestimation_of.md)

## isolation (1)

- [ ] `5097cbcb38e6` sched/isolation: Prevent boot crash when the boot CPU is nohz_full — [`5097cbcb_sched_isolation_prevent_boot_crash.md`](bugs/5097cbcb_sched_isolation_prevent_boot_crash.md)

## core (core-scheduling with throttling) (1)

- [ ] `530bfad1d53d` sched/core: Avoid selecting the task that is throttled to run when core-sched enable — [`530bfad1_avoid_selecting_the_task_that.md`](bugs/530bfad1_avoid_selecting_the_task_that.md)

## core (CPU hotplug, task migration) (1)

- [ ] `5ba2ffba13a1` sched: Fix CPU hotplug / tighten is_per_cpu_kthread() — [`5ba2ffba_cpu_hotplug_tighten_is_per_cpu_kthread.md`](bugs/5ba2ffba_cpu_hotplug_tighten_is_per_cpu_kthread.md)

## NUMA balancing (scheduler) (1)

- [ ] `5c7b1aaf139d` sched/numa: Avoid migrating task to CPU-less node — [`5c7b1aaf_avoid_migrating_task_cpuless_node.md`](bugs/5c7b1aaf_avoid_migrating_task_cpuless_node.md)

## NUMA, core scheduler (1)

- [ ] `5f1b64e9a9b7` sched/numa: fix memory leak due to the overwritten vma->numab_state — [`5f1b64e9_memory_leak_due_overwritten_vmanumab_state.md`](bugs/5f1b64e9_memory_leak_due_overwritten_vmanumab_state.md)

## EXT (Energy-Aware Scheduling) (1)

- [ ] `619e090c8e40` sched/fair: Fix negative energy delta in find_energy_efficient_cpu() — [`619e090c_negative_energy_delta_find_energy_efficient_cpu.md`](bugs/619e090c_negative_energy_delta_find_energy_efficient_cpu.md)

## deadline scheduler, core scheduler (1)

- [ ] `6455ad5346c9` sched: Move sched_class::prio_changed() into the change pattern — [`6455ad53_move_sched_classprio_changed_into_change_pattern.md`](bugs/6455ad53_move_sched_classprio_changed_into_change_pattern.md)

## Deadline scheduler (1)

- [ ] `64e6fa76610e` sched/deadline: Fix potential race in dl_add_task_root_domain() — [`64e6fa76_potential_race_dl_add_task_root_domain.md`](bugs/64e6fa76_potential_race_dl_add_task_root_domain.md)

## CFS (eEVDF) (1)

- [ ] `650cad561cce` sched/eevdf: Fix avg_vruntime() — [`650cad56_avg_vruntime.md`](bugs/650cad56_avg_vruntime.md)

## Isolation (1)

- [ ] `65e53f869e9f` sched/isolation: Fix housekeeping_mask memory leak — [`65e53f86_housekeeping_mask_memory_leak.md`](bugs/65e53f86_housekeeping_mask_memory_leak.md)

## core, SMP (1)

- [ ] `68f4ff04dbad` sched, smp: Trace smp callback causing an IPI — [`68f4ff04_smp_trace_smp_callback_causing.md`](bugs/68f4ff04_smp_trace_smp_callback_causing.md)

## EXT (sched_ext / eBPF scheduler) (1)

- [ ] `69d5e722be94` sched/ext: Fix scx vs sched_delayed — [`69d5e722_schedext_fix_scx_sched_delayed.md`](bugs/69d5e722_schedext_fix_scx_sched_delayed.md)

## CFS (EEVDF) (1)

- [ ] `6d71a9c61604` Fix EEVDF entity placement bug causing scheduling lag — [`6d71a9c6_eevdf_entity_placement_bug_causing.md`](bugs/6d71a9c6_eevdf_entity_placement_bug_causing.md)

## uclamp (1)

- [ ] `7226017ad37a` sched/uclamp: Fix a bug in propagating uclamp value in new cgroups — [`7226017a_a_bug_in_propagating_uclamp.md`](bugs/7226017a_a_bug_in_propagating_uclamp.md)

## Deadline, core (1)

- [ ] `740797ce3a12` Fix PI boosting between RT and DEADLINE tasks — [`740797ce_pi_boosting_between_rt_and.md`](bugs/740797ce_pi_boosting_between_rt_and.md)

## fair (NUMA scheduling) (1)

- [ ] `76c389ab2b5e` sched/fair: Fix kernel build warning in test_idle_cores() for !SMT NUMA — [`76c389ab_kernel_build_warning_in_testidlecores.md`](bugs/76c389ab_kernel_build_warning_in_testidlecores.md)

## cputime (1)

- [ ] `77baa5bafcbe` sched/cputime: Fix mul_u64_u64_div_u64() precision for cputime — [`77baa5ba_mulu64u64divu64_precision_for_cputime.md`](bugs/77baa5ba_mulu64u64divu64_precision_for_cputime.md)

## CFS/EEVDF (Earliest Eligible Virtual Deadline First) (1)

- [ ] `79f3f9bedd14` sched/eevdf: Fix min_vruntime vs avg_vruntime — [`79f3f9be_minvruntime_vs_avgvruntime.md`](bugs/79f3f9be_minvruntime_vs_avgvruntime.md)

## Core scheduling (core-wide task selection) (1)

- [ ] `7afbba119f0d` sched: Fix priority inversion of cookied task with sibling — [`7afbba11_priority_inversion_of_cookied_task.md`](bugs/7afbba11_priority_inversion_of_cookied_task.md)

## PSI (Pressure Stall Information), Core Scheduler (1)

- [ ] `7d9da040575b` psi: Fix race when task wakes up before psi_sched_switch() adjusts flags — [`7d9da040_race_when_task_wakes_up.md`](bugs/7d9da040_race_when_task_wakes_up.md)

## Deadline scheduling (1)

- [ ] `7ea98dfa4491` sched/deadline: Add more reschedule cases to prio_changed_dl() — [`7ea98dfa_add_more_reschedule_cases_to.md`](bugs/7ea98dfa_add_more_reschedule_cases_to.md)

## membarrier (1)

- [ ] `809232619f5b` Fix membarrier-rseq fence command missing from query bitmask — [`80923261_membarrier_rseq_fence_command_missing.md`](bugs/80923261_membarrier_rseq_fence_command_missing.md)

## autogroup (core scheduler) (1)

- [ ] `82f586f923e3` sched/autogroup: Fix sysctl move — [`82f586f9_schedautogroup_fix_sysctl_move.md`](bugs/82f586f9_schedautogroup_fix_sysctl_move.md)

## NUMA, fair scheduling (1)

- [ ] `84db47ca7146` Fix mm numa_scan_seq based unconditional scan — [`84db47ca_fix_mm_numa_scan_seq.md`](bugs/84db47ca_fix_mm_numa_scan_seq.md)

## core (run-queue balance callback) (1)

- [ ] `868ad33bfa3b` sched: Prevent balance_push() on remote runqueues — [`868ad33b_prevent_balance_push_on_remote.md`](bugs/868ad33b_prevent_balance_push_on_remote.md)

## EXT (extensible scheduler) (1)

- [ ] `8a9b1585e2bf` sched_ext: Validate prev_cpu in scx_bpf_select_cpu_dfl() — [`8a9b1585_validate_prev_cpu_scx_bpf.md`](bugs/8a9b1585_validate_prev_cpu_scx_bpf.md)

## CFS (EEVDF scheduling) (1)

- [ ] `8dafa9d0eb1a` sched/eevdf: Fix min_deadline heap integrity — [`8dafa9d0_fix_min_deadline_heap_integrity.md`](bugs/8dafa9d0_fix_min_deadline_heap_integrity.md)

## EXT (cpufreq scheduling utility) (1)

- [ ] `8e461a1cb43d` cpufreq: schedutil: Fix superfluous updates caused by need_freq_update — [`8e461a1c_schedutil_fix_superfluous_updates_caused.md`](bugs/8e461a1c_schedutil_fix_superfluous_updates_caused.md)

## core (uclamp/cgroup) (1)

- [ ] `93b73858701f` sched/uclamp: Fix locking around cpu_util_update_eff() — [`93b73858_fix_locking_around_cpu_util.md`](bugs/93b73858_fix_locking_around_cpu_util.md)

## EXT (Extensible Scheduler Class) (1)

- [ ] `9753358a6a2b` sched_ext: Fix SCX_TASK_INIT -> SCX_TASK_READY transitions in scx_ops_enable() — [`9753358a_fix_scx_task_init_scx.md`](bugs/9753358a_fix_scx_task_init_scx.md)

## core, fair scheduling, delayed dequeue (1)

- [ ] `98442f0ccd82` sched: Fix delayed_dequeue vs switched_from_fair() — [`98442f0c_fix_delayed_dequeue_vs_switched.md`](bugs/98442f0c_fix_delayed_dequeue_vs_switched.md)

## CFS (fair scheduling), PELT clock (1)

- [ ] `98c88dc8a1ac` sched/fair: Fix pelt clock sync when entering idle — [`98c88dc8_fix_pelt_clock_sync_when.md`](bugs/98c88dc8_fix_pelt_clock_sync_when.md)

## sched/mmcid (1)

- [ ] `9da6ccbcea3d` sched/mmcid: Implement deferred mode change — [`9da6ccbc_implement_deferred_mode_change.md`](bugs/9da6ccbc_implement_deferred_mode_change.md)

## cpufreq schedutil (1)

- [ ] `9e0bc36ab07c` cpufreq: schedutil: Update next_freq when cpufreq_limits change — [`9e0bc36a_schedutil_update_next_freq_when.md`](bugs/9e0bc36a_schedutil_update_next_freq_when.md)

## Core (cpuidle/RCU interaction) (1)

- [ ] `a01353cf1896` cpuidle: Fix ct_idle_*() usage — [`a01353cf_fix_ct_idle_usage.md`](bugs/a01353cf_fix_ct_idle_usage.md)

## Core scheduler tracing (1)

- [ ] `a1bd06853ee4` sched: Fix use of count for nr_running tracepoint — [`a1bd0685_fix_use_of_count_for.md`](bugs/a1bd0685_fix_use_of_count_for.md)

## EXT (sched_ext, sched/core) (1)

- [ ] `a1eab4d813f7` sched_ext, sched/core: Fix build failure when !FAIR_GROUP_SCHED && EXT_GROUP_SCHED — [`a1eab4d8_sched_ext_sched_core_fix.md`](bugs/a1eab4d8_sched_ext_sched_core_fix.md)

## EXT (sched_ext scheduler class) (1)

- [ ] `a3c4a0a42e61` sched_ext: fix flag check for deferred callbacks — [`a3c4a0a4_fix_flag_check_for_deferred.md`](bugs/a3c4a0a4_fix_flag_check_for_deferred.md)

## core (scheduler debug) (1)

- [ ] `a6fcdd8d95f7` sched/debug: Correct printing for rq->nr_uninterruptible — [`a6fcdd8d_correct_printing_for_rq_nr.md`](bugs/a6fcdd8d_correct_printing_for_rq_nr.md)

## Core scheduler (1)

- [ ] `a73f863af4ce` sched/features: Fix !CONFIG_JUMP_LABEL case — [`a73f863a_fix_config_jump_label_case.md`](bugs/a73f863a_fix_config_jump_label_case.md)

## RT, Deadline, core (1)

- [ ] `a7c81556ec4d` sched: Fix migrate_disable() vs rt/dl balancing — [`a7c81556_fix_migrate_disable_vs_rt.md`](bugs/a7c81556_fix_migrate_disable_vs_rt.md)

## Core scheduler debug (1)

- [ ] `ad789f84c9a1` sched/debug: Fix cgroup_path[] serialization — [`ad789f84_fix_cgroup_path_serialization.md`](bugs/ad789f84_fix_cgroup_path_serialization.md)

## CFS (Completely Fair Scheduler) / EEVDF (1)

- [ ] `afae8002b4fd` sched/eevdf: Fix miscalculation in reweight_entity() when se is not curr — [`afae8002_fix_miscalculation_in_reweight_entity.md`](bugs/afae8002_fix_miscalculation_in_reweight_entity.md)

## EDF (Earliest Deadline First scheduling) (1)

- [ ] `b01db23d5923` sched/eevdf: Fix pick_eevdf() — [`b01db23d_fix_pick_eevdf.md`](bugs/b01db23d_fix_pick_eevdf.md)

## CFS (task group scheduling) (1)

- [ ] `b027789e5e50` sched/fair: Prevent dead task groups from regaining cfs_rq's — [`b027789e_prevent_dead_task_groups_from.md`](bugs/b027789e_prevent_dead_task_groups_from.md)

## PSI (Pressure Stall Information), core scheduler (1)

- [ ] `b05e75d61138` psi: Fix cpu.pressure for cpu.max and competing cgroups — [`b05e75d6_fix_cpu_pressure_for_cpu.md`](bugs/b05e75d6_fix_cpu_pressure_for_cpu.md)

## Core scheduler (DVFS/cpufreq integration) (1)

- [ ] `b3edde44e5d4` cpufreq/schedutil: Use a fixed reference frequency — [`b3edde44_cpufreq_schedutil_use_a_fixed.md`](bugs/b3edde44_cpufreq_schedutil_use_a_fixed.md)

## Deadline scheduling, cpuset integration (1)

- [ ] `b6e8d40d43ae` sched, cpuset: Fix dl_cpu_busy() panic due to empty cs->cpus_allowed — [`b6e8d40d_sched_cpuset_fix_dl_cpu.md`](bugs/b6e8d40d_sched_cpuset_fix_dl_cpu.md)

## CFS (util_est) (1)

- [ ] `b89997aa88f0` sched/pelt: Fix task util_est update filtering — [`b89997aa_task_util_est_update_filtering.md`](bugs/b89997aa_task_util_est_update_filtering.md)

## core (rseq, memory map concurrency IDs) (1)

- [ ] `bbd0b031509b` sched/rseq: Fix concurrency ID handling of usermodehelper kthreads — [`bbd0b031_concurrency_id_handling_of_usermodehelper.md`](bugs/bbd0b031_concurrency_id_handling_of_usermodehelper.md)

## core (proxy execution with RT and deadline) (1)

- [ ] `be39617e38e0` sched: Fix proxy/current (push,pull)ability — [`be39617e_proxy_current_push_pull_ability.md`](bugs/be39617e_proxy_current_push_pull_ability.md)

## PSI (Pressure Stall Information), core scheduling (1)

- [ ] `c6508124193d` sched/psi: Fix mistaken CPU pressure indication after corrupted task state bug — [`c6508124_mistaken_cpu_pressure_indication_after.md`](bugs/c6508124_mistaken_cpu_pressure_indication_after.md)

## Deadline, Fair scheduling (1)

- [ ] `c7f7e9c73178` sched/dlserver: Fix dlserver time accounting — [`c7f7e9c7_dlserver_time_accounting.md`](bugs/c7f7e9c7_dlserver_time_accounting.md)

## Deadline scheduling, core scheduling (1)

- [ ] `c8a85394cfdb` Fix picking of tasks for core scheduling with DL server — [`c8a85394_picking_of_tasks_for_core.md`](bugs/c8a85394_picking_of_tasks_for_core.md)

## core, topology (1)

- [ ] `cab3ecaed5cd` Fixed missing rq clock update before calling set_rq_offline() — [`cab3ecae_fixed_missing_rq_clock_update.md`](bugs/cab3ecae_fixed_missing_rq_clock_update.md)

## CFS (load balancing, load balance statistics) (1)

- [ ] `cd18bec668bb` Fix update of rd->sg_overutilized — [`cd18bec6_update_of_rd_sg_overutilized.md`](bugs/cd18bec6_update_of_rd_sg_overutilized.md)

## CPUfreq (1)

- [ ] `cdcc5ef26b39` Revert "cpufreq: schedutil: Move max CPU capacity to sugov_policy" — [`cdcc5ef2_revert_cpufreq_sugov_policy.md`](bugs/cdcc5ef2_revert_cpufreq_sugov_policy.md)

## core (membarrier) (1)

- [ ] `ce29ddc47b91` sched/membarrier: fix missing local execution of ipi_sync_rq_state() — [`ce29ddc4_membarrier_ipi_sync_rq.md`](bugs/ce29ddc4_membarrier_ipi_sync_rq.md)

## cpufreq/sched (1)

- [ ] `cfde542df7dd` cpufreq/sched: Fix the usage of CPUFREQ_NEED_UPDATE_LIMITS — [`cfde542d_fix_cpufreq_need_update_limits.md`](bugs/cfde542d_fix_cpufreq_need_update_limits.md)

## CFS (Fair Scheduling), EEVDF (1)

- [ ] `d2929762cc3f` sched/eevdf: Fix heap corruption more — [`d2929762_eevdf_fix_heap_corruption.md`](bugs/d2929762_eevdf_fix_heap_corruption.md)

## cpufreq, uclamp (1)

- [ ] `d37aee9018e6` sched/uclamp: Fix iowait boost escaping uclamp restriction — [`d37aee90_uclamp_iowait_boost_escape.md`](bugs/d37aee90_uclamp_iowait_boost_escape.md)

## CFS and RT bandwidth management (1)

- [ ] `d505b8af5891` sched: Defend cfs and rt bandwidth quota against overflow — [`d505b8af_defend_bandwidth_overflow.md`](bugs/d505b8af_defend_bandwidth_overflow.md)

## uclamp (utilization clamping) (1)

- [ ] `d81ae8aac85c` sched/uclamp: Fix initialization of struct uclamp_rq — [`d81ae8aa_uclamp_rq_initialization.md`](bugs/d81ae8aa_uclamp_rq_initialization.md)

## EAS (Energy-Aware Scheduling), CFS (1)

- [ ] `da0777d35f47` Fix wrong negative conversion in find_energy_efficient_cpu() — [`da0777d3_fix_negative_conversion_energy_cpu.md`](bugs/da0777d3_fix_negative_conversion_energy_cpu.md)

## CFS (Capacity-aware scheduling) (1)

- [ ] `da07d2f9c153` sched/fair: Fixes for capacity inversion detection — [`da07d2f9_fair_capacity_inversion_detection.md`](bugs/da07d2f9_fair_capacity_inversion_detection.md)

## Deadline, CFS (1)

- [ ] `dae4320b29f0` sched: Fixup set_next_task() implementations — [`dae4320b_fixup_set_next_task.md`](bugs/dae4320b_fixup_set_next_task.md)

## NUMA balancing (1)

- [ ] `db6cc3f4ac2e` Revert "sched/numa: add statistics of numa balance task" — [`db6cc3f4_revert_numa_balance_stats.md`](bugs/db6cc3f4_revert_numa_balance_stats.md)

## sched/smt (1)

- [ ] `e22f910a26cc` sched/smt: Fix unbalance sched_smt_present dec/inc — [`e22f910a_smt_unbalance_present_dec_inc.md`](bugs/e22f910a_smt_unbalance_present_dec_inc.md)

## EXT (energy-aware scheduling), uclamp (1)

- [ ] `e26fd28db828` sched/uclamp: Fix a uninitialized variable warnings — [`e26fd28d_uclamp_uninitialized_var.md`](bugs/e26fd28d_uclamp_uninitialized_var.md)

## sched_ext (scheduler extension), CFS group scheduling (1)

- [ ] `e4e149dd2f80` sched_ext: Merge branch 'for-6.16-fixes' into for-6.17 — [`e4e149dd_sched_ext_merge_6_16_to_6_17.md`](bugs/e4e149dd_sched_ext_merge_6_16_to_6_17.md)

## Deadline (sched/deadline) (1)

- [ ] `e636ffb9e31b` sched/deadline: Fix dl_server time accounting — [`e636ffb9_fix_dl_server_time_accounting.md`](bugs/e636ffb9_fix_dl_server_time_accounting.md)

## Core scheduler (RT/migration) (1)

- [ ] `e681dcbaa4b2` sched: Fix get_push_task() vs migrate_disable() — [`e681dcba_fix_get_push_task_vs_migrate_disable.md`](bugs/e681dcba_fix_get_push_task_vs_migrate_disable.md)

## Core scheduling (1)

- [ ] `e705968dd687` Fix comparison in sched_group_cookie_match() — [`e705968d_fix_comparison_sched_group_cookie_match.md`](bugs/e705968d_fix_comparison_sched_group_cookie_match.md)

## cputime/core (1)

- [ ] `e7f2be115f07` sched/cputime: Fix getrusage(RUSAGE_THREAD) with nohz_full — [`e7f2be11_fix_getrusagerusage_thread_nohz_full.md`](bugs/e7f2be11_fix_getrusagerusage_thread_nohz_full.md)

## core (housekeeping/CPU isolation) (1)

- [ ] `e894f6339808` kthread: Honour kthreads preferred affinity after cpuset changes — [`e894f633_honour_kthreads_preferred_affinity_after.md`](bugs/e894f633_honour_kthreads_preferred_affinity_after.md)

## CFS (EDF scheduling policy) (1)

- [ ] `eab03c23c2a1` sched/eevdf: Fix vruntime adjustment on reweight — [`eab03c23_fix_vruntime_adjustment_reweight.md`](bugs/eab03c23_fix_vruntime_adjustment_reweight.md)

## sched_ext (idle CPU selection) (1)

- [ ] `ee9a4e92799d` sched_ext: idle: Properly handle invalid prev_cpu during idle selection — [`ee9a4e92_idle_properly_handle_invalid_prev_cpu.md`](bugs/ee9a4e92_idle_properly_handle_invalid_prev_cpu.md)

## Core scheduler, fair scheduling, real-time scheduling, deadline scheduling (1)

- [ ] `f0498d2a54e7` sched: Fix stop_one_cpu_nowait() vs hotplug — [`f0498d2a_fix_stop_one_cpu_nowait_vs_hotplug.md`](bugs/f0498d2a_fix_stop_one_cpu_nowait_vs_hotplug.md)

## sched/numa (1)

- [ ] `f22cde4371f3` sched/numa: Fix the vma scan starving issue — [`f22cde43_fix_vma_scan_starving_issue.md`](bugs/f22cde43_fix_vma_scan_starving_issue.md)

## clock (1)

- [ ] `f31dcb152a3d` sched/clock: Fix local_clock() before sched_clock_init() — [`f31dcb15_fix_local_clock_before_sched_clock_init.md`](bugs/f31dcb15_fix_local_clock_before_sched_clock_init.md)

## EXT (sched_ext scheduler) (1)

- [ ] `f39489fea677` sched_ext: add a missing rcu_read_lock/unlock pair at scx_select_cpu_dfl() — [`f39489fe_add_missing_rcu_read_lockunlock_pair_scx_select_cpu_dfl.md`](bugs/f39489fe_add_missing_rcu_read_lockunlock_pair_scx_select_cpu_dfl.md)

## CFS (Fair Scheduling with EEVDF) (1)

- [ ] `fc1892becd56` sched/eevdf: Fixup PELT vs DELAYED_DEQUEUE — [`fc1892be_fixup_pelt_vs_delayed_dequeue.md`](bugs/fc1892be_fixup_pelt_vs_delayed_dequeue.md)

## core scheduler (mm_cid context tracking) (1)

- [ ] `fe90f3967bdb` sched: Add missing memory barrier in switch_mm_cid — [`fe90f396_add_missing_memory_barrier_switch_mm_cid.md`](bugs/fe90f396_add_missing_memory_barrier_switch_mm_cid.md)

## RT, Deadline (1)

- [ ] `feffe5bb274d` sched/rt: Fix bad task migration for rt tasks — [`feffe5bb_fix_bad_task_migration_for.md`](bugs/feffe5bb_fix_bad_task_migration_for.md)

