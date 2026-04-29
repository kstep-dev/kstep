# Unplanned Bugs
| Commit | Subsystem | Title | Reason |
|--------|-----------|-------|--------|
| `ee6e44dfe6e5` | deadline | dl_server hrtimer fires after CPU goes offline | Requires CPU hotplug teardown sequence (sched_cpu_dying path) which kSTEP cannot simulate |
| `db6cc3f4ac2e` | numa | NULL deref in __migrate_swap_task on task exit race | Requires real userspace processes with mm_struct, NUMA memory access patterns, and process exit lifecycle — kSTEP only has kernel threads |
| `009836b4fa52` | core | migrate_swap vs hotplug stopper wakeup race | Requires CPU hotplug (_cpu_down → balance_push) and NUMA balancing migrate_swap — kSTEP cannot simulate CPU hotplug events |
| `79443a7e9da3` | cpufreq | Missing memory barriers for schedutil limits_changed flag | Requires real cpufreq driver (QEMU has none) and CPU memory reordering (QEMU/TCG is sequentially consistent) |
| `cfde542df7dd` | cpufreq | Schedutil skips policy limits for non-NEED_UPDATE_LIMITS drivers | Requires a registered cpufreq driver and active schedutil governor — QEMU has no cpufreq hardware or driver |
| `02d954c0fdf9` | core | RSEQ mm_cid fails to compact after thread/affinity reduction | Requires real userspace threads sharing mm_struct with RSEQ — kSTEP only has kernel threads with no mm |
| `8e461a1cb43d` | cpufreq | Superfluous freq updates from need_freq_update never cleared | Requires a registered cpufreq driver and active schedutil governor — QEMU has no cpufreq hardware or driver |
| `a430d99e3490` | lb | Incorrect schedstat accounting for hot task migrations | Pure statistics-accounting bug; requires per-task migration-decision hooks inside detach_tasks() and precise exec_start control to verify counter correctness |
| `5f1b64e9a9b7` | numa | Memory leak from concurrent vma->numab_state init | Requires real userspace threads sharing mm_struct/VMAs and task_numa_work callback via syscall exit — kSTEP only has kernel threads with no mm |
| `ea9cffc0a154` | core | Spurious need_resched() skips nohz idle load balance | Requires TIF_POLLING_NRFLAG during nohz idle (mwait-based x86 idle); QEMU TCG uses hlt which clears the flag |
| `9c70b2a33cd2` | numa | NULL deref in task_numa_work on empty address space | Requires real userspace process with mm_struct that unmaps entire address space via munmap() — kSTEP only has kernel threads with no mm |
| `22368fe1f9bb` | deadline | Bitwise AND typo in deferrable DL server replenishment | No observable behavioral difference — `&` and `&&` produce identical results for 1-bit bitfields; code-quality fix only |
| `70d8b6485b0b` | cpufreq | EAS sched-domain rebuild skipped on shared-tunables path | Requires cpufreq driver registration and governor init lifecycle — QEMU has no cpufreq hardware or driver |
| `73ab05aa46b0` | core | Sleeping-in-atomic in task_tick_mm_cid() KASAN page alloc | Requires CONFIG_KASAN + CONFIG_PREEMPT_RT; bug is a locking violation (not scheduling behavior), unobservable via kSTEP |
| `cd9626e9ebc7` | core | External p->on_rq users misclassify delayed-dequeue tasks | Bug manifests only in external subsystems (KVM, perf, freezer, RCU, ftrace) not accessible from kSTEP; scheduler behavior is identical on buggy and fixed kernels |
| `f22cde4371f3` | numa | VMA scan starvation due to cleared pids_active | Requires real userspace processes with mm_struct/VMAs, NUMA page fault infrastructure (prot_none), and real NUMA memory topology — kSTEP only has kernel threads with no mm |
| `5ebde09d9170` | core | RQCF_ACT_SKIP flag leak from __schedule() | Requires non-deterministic cross-CPU race between newidle_balance() lock drop and cgroup bandwidth destruction; kSTEP lacks cgroup bandwidth/destroy APIs and cannot orchestrate the timing |
| `fe7a11c78d2a` | core | Missing set_rq_online rollback on failed CPU deactivation | Requires CPU hotplug (sched_cpu_deactivate path) with DL bandwidth overflow to trigger cpuset_cpu_inactive() failure — kSTEP cannot simulate CPU hotplug |
| `77baa5bafcbe` | core | cputime_adjust unsigned overflow from mul_u64_u64_div_u64() approximation | Bug only manifests on architectures using generic mul_u64_u64_div_u64() fallback (e.g. ARM64); x86_64 has exact native implementation, so bug cannot occur in kSTEP |
| `5097cbcb38e6` | core | Boot crash when boot CPU is nohz_full | Boot-time crash before smp_init(); kSTEP modules load after boot completes and cannot inject nohz_full= boot parameters |
| `e37617c8e53a` | cpufreq | Frequency stuck at current OPP on non-invariant systems | Requires registered cpufreq driver and active schedutil governor — QEMU has no cpufreq hardware or driver |
| `be3a51e68f2f` | energy | Unnecessary overutilized writes cause cache contention | Pure performance bug (cache line false sharing); no scheduling behavior difference — requires real hardware cache coherency to observe |
| `f0498d2a54e7` | core | stop_one_cpu_nowait() vs CPU hotplug preemption race | Requires CPU hotplug (_cpu_down → stop_machine_park → stopper->enabled=false) which kSTEP cannot simulate |
| `c0490bc9bb62` | bandwidth | cfs_rq_is_decayed() always true on !SMP breaks leaf list | Bug is in `#else /* CONFIG_SMP */` stub; kSTEP always builds with CONFIG_SMP=y so buggy code is never compiled |
| `f8858d96061f` | lb | Redundant SMT sibling iteration in should_we_balance() | Pure performance regression (excess iterations); scheduling decisions identical on buggy/fixed kernels — no observable behavioral difference |
| `6b00a4014765` | energy | EAS skips CPUs with zero spare capacity under uclamp_max | Requires EAS active (cpufreq driver + schedutil governor + energy model) — QEMU has no cpufreq hardware or driver |
| `a57415f5d1e4` | deadline | Incorrect per-CPU vs per-root-domain DL bandwidth comparison | Fix targets v5.11-rc1 (pre-v5.15); kSTEP supports v5.15+ only, where the bug is already fixed |
| `c2e164ac33f7` | pelt | util_est boosting double-counts waking task | Bug only triggers through EAS code paths (find_energy_efficient_cpu); requires active energy model + frequency invariance — QEMU has no cpufreq hardware or driver |
| `f31dcb152a3d` | core | local_clock() wrong before sched_clock_init() | Bug occurs during very early boot before sched_clock_init(); kSTEP loads as a module after boot completes — buggy code path unreachable |
| `01bb11ad828b` | topology | KASAN OOB read in hop_cmp() NUMA lookup | No scheduling behavior difference — bug is a KASAN-detected out-of-bounds heap read; correct results on both buggy and fixed kernels |
| `223baf9d17f2` | core | mm_cid lock contention performance regression | Performance regression (lock contention); requires real userspace processes with mm_struct and many CPUs — kSTEP only has kernel threads with no mm |
| `5657c1167835` | core | NULL deref in sched_setaffinity() on non-SMP | Bug is in `!CONFIG_SMP` code path; kSTEP always builds with CONFIG_SMP=y so buggy code is never compiled |
| `7c4a5b89a0b5` | rt | Dead BUG_ON in pick_next_rt_entity misses empty queue | Defensive coding fix — BUG_ON checks impossible condition (list_entry returning NULL); actual error (bitmap/queue desync) cannot be triggered through normal scheduling operations |
| `da07d2f9c153` | energy | Capacity inversion detection missing RCU lock and self-comparison | Requires EAS active (energy model + schedutil governor + performance domains) and thermal pressure injection — QEMU has no cpufreq hardware, thermal drivers, or energy model |
| `7fb3ff22ad87` | core | arch_scale_freq_tick() overflow on tickless CPUs | Requires x86 APERF/MPERF hardware MSRs and CONFIG_NO_HZ_FULL with ~30 min tickless period — QEMU/TCG does not emulate these MSRs |
| `9a5418bc48ba` | core | kfree() under pi_lock sleeps on PREEMPT_RT | Requires CONFIG_PREEMPT_RT kernel and CPU hotplug; bug is lockdep/sleeping-in-atomic warning, not scheduling logic |
| `87ca4f9efbd7` | core | Use-after-free in dup_user_cpus_ptr() during fork race | Memory safety bug (UAF) requiring concurrent fork() + do_set_cpus_allowed() race; kSTEP lacks sched_setaffinity support and cannot detect memory corruption |
| `e705968dd687` | core | sched_group_cookie_match checks wrong rq | Requires core scheduling (prctl PR_SCHED_CORE) to set task cookies — kSTEP cannot issue prctl syscalls or safely enable core scheduling internals |
| `a2e7f03ed28f` | uclamp | asym_fits_capacity ignores uclamp migration margin | Requires asymmetric CPU capacity (different capacity_orig_of values); x86 pre-6.12 has no per-CPU capacity variable — capacity_orig_of() is a compile-time constant |
| `5aec788aeb8e` | core | TASK_state bitmask comparisons broken by TASK_FREEZABLE | Requires freezable wait macros (wait_event_freezable) with signal delivery and freezer subsystem; kSTEP cannot run custom kthread code or send signals |
| `9c2136be0878` | core | sched_switch tracepoint arg reorder breaks BPF | Tracepoint ABI issue affecting BPF programs; kSTEP cannot load BPF programs or invoke the bpf() syscall to test tracepoint argument type verification |
| `04193d590b39` | core | balance_push() vs __sched_setscheduler() race hangs CPU hotplug | Requires CPU hotplug offline flow (balance_push_callback) racing with sched_setscheduler from another CPU; kSTEP has no CPU hotplug support |
| `b3f9916d81e8` | numa | NULL mm deref in task_tick_numa after PF_KTHREAD rework | Requires a task without PF_KTHREAD and without mm (init pre-execve state); kSTEP cannot create user_mode_thread tasks |
| `7f434dff7621` | topology | Redundant variable and missing __rcu annotation in build_sched_domains | No runtime behavioral difference — purely a sparse static analysis warning and redundant variable cleanup; kSTEP tests runtime behavior |
| `386ef214c3c6` | core | Forced-newidle balancer steals migration-disabled task | Requires core scheduling (prctl PR_SCHED_CORE) to set task cookies — kSTEP cannot issue prctl syscalls or safely enable core scheduling internals |
| `ab31c7fd2d37` | numa | Boot crash from NUMA_NO_NODE in task_numa_placement | Requires userspace processes with mm_struct for NUMA balancing; task_numa_placement() is static and only reachable via page fault scanning — kSTEP only has kernel threads |
| `d37aee9018e6` | uclamp | iowait boost escapes uclamp_max restriction | Requires active schedutil cpufreq governor and real I/O wait workloads — QEMU has no cpufreq driver and kSTEP cannot trigger iowait_boost |
| `d8fcb81f1acf` | fair | wake_affine ignores idle prev_cpu across sockets | Fix targets v5.11-rc1; kSTEP requires v5.15+ so no buggy kernel version is available |
| `7a17e1db1265` | cpufreq | Schedutil busy filter ignores uclamp_max capping | Requires registered cpufreq driver and active schedutil governor — QEMU has no cpufreq hardware or driver |
| `13765de8148f` | fair | NULL deref in reweight_entity during fork/setpriority race | Requires concurrent clone(CLONE_THREAD) + setpriority(PRIO_PGRP) from real userspace threads sharing a thread group — kSTEP only creates separate processes |
| `b1e8206582f9` | core | Fork race exposes incompletely initialized task to syscalls | Requires real userspace fork() + setpriority(PRIO_PGRP) syscalls racing; kSTEP uses kthreads which follow a different creation path |
| `63acd42c0d49` | core | Shadow Call Stack overflow on CPU hotplug | Requires ARM64 with CONFIG_SHADOW_CALL_STACK (x86 has no SCS) and CPU hotplug lifecycle — kSTEP runs x86_64 only |
| `dce1ca0525bf` | core | Stale SCS/KASAN stack state across CPU hotplug | Requires CPU hotplug (offline/online cycling) which kSTEP cannot simulate — no kstep_cpu_offline/online API exists |
| `9ed20bafc858` | core | Dynamic preempt __setup() returns swapped values | Bug is in __init boot-time callback for preempt= parameter; kSTEP modules load after boot and cannot trigger or observe __setup() behavior |
| `703066188f63` | debug | Null-terminated buffer missing in tunable_scaling write | Debugfs input parsing bug (missing null terminator); no scheduler logic error — requires userspace file I/O to debugfs which kSTEP cannot perform |
| `e681dcbaa4b2` | rt | get_push_task() missing migrate_disable() check | Fix targets v5.14 (pre-v5.15); bug requires CONFIG_PREEMPT_RT and migrate_disable() API — kSTEP supports v5.15+ only |
| `868ad33bfa3b` | core | balance_push() invoked on remote CPU during hotplug | Requires CPU hotplug (cpu_dying state) which kSTEP cannot simulate; no kstep_cpu_offline() API exists |
| `014ba44e8184` | uclamp | Per-CPU kthread wakee stacking ignores capacity on asym systems | Requires asymmetric CPU capacity (ARM big.LITTLE); x86 gained asym support in v6.12 but bug was fixed in v5.17 — no kernel exists with both the bug and x86 asym capacity |
| `3c474b3239f1` | core | NULL rq->core dereference on uninitialized CPUs | Bug fixed in v5.14 (before kSTEP minimum v5.15); also requires possible-but-never-online CPUs which kSTEP cannot simulate |
| `e5c6b312ce3c` | cpufreq | Premature kfree of sugov_tunables bypassing kobject release | Requires registered cpufreq driver and active schedutil governor — QEMU has no cpufreq hardware or driver |
| `f558c2b834ec` | rt | Double enqueue from rt_effective_prio race in setscheduler | Fix targets v5.14-rc5 (pre-v5.15); kSTEP supports v5.15+ only — bug already fixed before v5.15 |
| `1c6829cfd3d5` | numa | is_core_idle() checks wrong CPU variable in SMT loop | Kernel version too old (v5.13-rc6 at parent commit, pre-v5.15); also requires real userspace processes with mm_struct for NUMA balancing |
| `02dbb7246c5b` | lb | has_idle_cores flag cleared on wrong CPU's LLC | Kernel version too old (fix in v5.13-rc2, pre-v5.15); kSTEP supports v5.15+ only |
| `3e1493f46390` | uclamp | Stale max aggregation on idle runqueue | Kernel version too old (fix in v5.14-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `d7d607096ae6` | deadline | DL utilization tracking stale during policy change | Kernel version too old (fix in v5.14-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `ceb6ba45dc80` | pelt | dequeue_load_avg load_sum/load_avg desync | Kernel version too old (fix in v5.14-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `771fac5e26c1` | cpufreq | CPPC frequency invariance work race on policy exit | Requires ACPI CPPC firmware (ARM64), registered cpufreq driver, and CPU hotplug — QEMU has no CPPC and kSTEP has no hotplug API |
| `fecfcbc288e9` | pelt | RT utilization spike on policy change | Kernel version too old (fix in v5.14-rc1, buggy kernel is v5.13-rc6); kSTEP supports v5.15+ only |
| `a7b359fc6a37` | bandwidth | Missing decayed cfs_rq on leaf list after unthrottle | Kernel version too old (fix in v5.13-rc7, pre-v5.15); kSTEP supports v5.15+ only |
| `68d7a190682a` | pelt | util_est UTIL_AVG_UNCHANGED flag leaks via LSB | Kernel version too old (fix in v5.13-rc6, pre-v5.15); kSTEP supports v5.15+ only |
| `fdaba61ef8a2` | bandwidth | Decayed parent cfs_rq skipped on leaf list during unthrottle | Kernel version too old (fix in v5.13, pre-v5.15); kSTEP supports v5.15+ only |
| `0c18f2ecfcc2` | uclamp | cpu.uclamp.min implemented as limit instead of protection | Kernel version too old (fix in v5.14-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `93b73858701f` | uclamp | Missing locking around cpu_util_update_eff() in css_online | Kernel version too old (fix in v5.14-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `475ea6c60279` | core | Deferred CPU pick in migration_cpu_stop() selects offline CPU | Kernel version too old (fix in v5.14-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `0258bdfaff5b` | pelt | Missing load decay due to cfs_rq not on leaf list | Kernel version too old (fix in v5.13-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `3f1bc119cd7f` | core | Redundant migration when task already on valid CPU | Kernel version too old (fix in v5.12-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `6d2f8909a5fa` | uclamp | Out-of-bound bucket ID access | Kernel version too old (fix in v5.13-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `8a6edb5257e2` | core | migration_cpu_stop() NULL task from uninitialized pending->arg | Kernel version too old (fix in v5.12-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `9e81889c7648` | core | affine_move_task() stopper list corruption from concurrent set_cpus_allowed_ptr() | Kernel version too old (fix in v5.12-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `39a2a6eb5c9b` | lb | Shift-out-of-bounds in load_balance() detach_tasks() | Kernel version too old (fix in v5.13-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `0ae78eec8aa6` | energy | Misfit status on pinned task inflates balance_interval | Kernel version too old (fix in v5.12-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `8c1f560c1ea3` | pelt | Stale CPU util_est during task dequeue | Kernel version too old (fix in v5.12-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `ae7927023243` | core | balance_switch() performance regression in finish_lock_switch() | Kernel version too old (fix in v5.11-rc1, pre-v5.15); also a performance regression requiring real benchmarking, not a correctness bug |
| `8d4d9c7b4333` | debug | Memory corruption from kfree of offset pointer in sd_ctl_doflags | Kernel version too old (fix in v5.10-rc4, pre-v5.15); also requires userspace procfs reads which kSTEP cannot perform |
| `16b0a7a1a0af` | lb | Load balancer fails to spread tasks within LLC | Kernel version too old (fix in v5.10-rc4, pre-v5.15); kSTEP supports v5.15+ only |
| `8e1ac4299a6e` | energy | Overutilized update for new tasks is a no-op due to flags overwrite | Kernel version too old (fix in v5.10-rc5, pre-v5.15); kSTEP supports v5.15+ only |
| `b4c9c9f15649` | energy | Asymmetric wakeup path skips prev/target CPU checks | Kernel version too old (fix in v5.10-rc4, pre-v5.15); kSTEP supports v5.15+ only |
| `d707faa64d03` | core | Missing completion for affine_move_task() waiters | Kernel version too old (fix in v5.11-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `d1e7c2996e98` | cpufreq | Schedutil skips HWP limit updates when target freq unchanged | Kernel version too old (fix in v5.10-rc2, pre-v5.15); kSTEP supports v5.15+ only |
| `df3cb4ea1fb6` | lb | select_idle_smt() selects CPUs outside scheduling domain | Kernel version too old (fix in v5.10-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `da0777d35f47` | energy | Unsigned underflow in EAS spare capacity calculation | Kernel version too old (fix in v5.10-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `a73f863af4ce` | core | sched_feat() toggle silently ignored without CONFIG_JUMP_LABEL | Kernel version too old (fix in v5.10-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `345a957fcc95` | core | do_sched_yield() calls schedule() with interrupts disabled | Kernel version too old (fix in v5.11-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `ec618b84f6e1` | core | nr_iowait decrement-before-increment race in ttwu() vs schedule() | Kernel version too old (fix in v5.10-rc5, pre-v5.15); kSTEP supports v5.15+ only |
| `a1bd06853ee4` | core | nr_running tracepoint reports wrong sign on dequeue | Kernel version too old (fix in v5.9-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `233e7aca4c8a` | numa | Wrong argument to adjust_numa_imbalance in task_numa_find_cpu | Kernel version too old (fix in v5.10-rc1, pre-v5.15); also requires real userspace processes with mm_struct for NUMA fault scanning |
| `d81ae8aac85c` | uclamp | uclamp_rq init zeros UCLAMP_MAX capping freq to 0 | Kernel version too old (fix in v5.9-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `e65855a52b47` | uclamp | Deadlock when enabling uclamp static key | Kernel version too old (fix in v5.9-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `ce9bc3b27f2a` | deadline | Uninitialized dl_boosted triggers WARN_ON | Kernel version too old (fix in v5.8-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `46609ce22703` | uclamp | Fast path performance regression from unconditional uclamp logic | Kernel version too old (fix in v5.9-rc1, pre-v5.15); also a performance regression requiring real benchmarking, not a correctness bug |
| `d136122f5845` | core | ptrace_freeze_traced races with __schedule state double-check | Kernel version too old (fix in v5.8-rc7, pre-v5.15); kSTEP supports v5.15+ only |
| `740797ce3a12` | core | Spurious DL boosting of RT task due to uninitialized deadline | Kernel version too old (fix in v5.8-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `b6e13e85829f` | core | ttwu() race due to stale task_cpu() load ordering | Kernel version too old (fix in v5.8-rc3, pre-v5.15); also requires weak memory ordering hardware unavailable in QEMU |
| `dbfb089d360b` | core | Loadavg accounting race in ttwu() vs schedule() | Kernel version too old (fix in v5.8-rc6, pre-v5.15); kSTEP supports v5.15+ only |
| `e21cf43406a1` | pelt | Runnable avg init causes false overload classification | Kernel version too old (fix in v5.8-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `9b1b234bb86b` | topology | Incorrect correction of non-topological SD_flags in sd_init() | Kernel version too old (fix in v5.9-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `3ea2f097b17e` | lb | NOHZ next_balance overwritten before remote CPUs updated | Kernel version too old (fix in v5.9-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `0621df315402` | numa | Missing RCU lock in update_numa_stats() idle core check | Kernel version too old (fix in v5.7-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `5a6d6a6ccb5f` | bandwidth | CFS period timer refills with scaled quota before scaling | Kernel version too old (fix in v5.8-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `6c8116c914b6` | lb | Wrong condition for avg_load calculation in wakeup path | Kernel version too old (fix in v5.7-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `b34cb07dde7c` | bandwidth | Leaf CFS RQ list corruption during enqueue with throttled hierarchy | Kernel version too old (fix in v5.7-rc7, pre-v5.15); kSTEP supports v5.15+ only |
| `eaf5a92ebde5` | uclamp | Reset-on-fork from RT incorrectly sets uclamp.min to max | Kernel version too old (fix in v5.7-rc3, pre-v5.15); kSTEP supports v5.15+ only |
| `5ab297bab984` | bandwidth | Throttled cfs_rq skips h_nr_running/load_avg updates on enqueue/dequeue | Kernel version too old (fix in v5.7-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `b562d1406499` | uclamp | Negative values bypass cpu_uclamp_write range check | Kernel version too old (fix in v5.6-rc2, pre-v5.15); kSTEP supports v5.15+ only |
| `6212437f0f60` | bandwidth | Stale runnable_avg on CFS bandwidth throttle/unthrottle | Kernel version too old (fix in v5.7-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `39f23ce07b93` | bandwidth | Incomplete leaf list maintenance in unthrottle_cfs_rq() | Kernel version too old (fix in v5.7-rc7, pre-v5.15); kSTEP supports v5.15+ only |
| `26a8b12747c9` | bandwidth | Race between CFS runtime distribution and assignment | Kernel version too old (fix in v5.7-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `dcd6dffb0a75` | uclamp | Incomplete rq::uclamp array initialization (only first element zeroed) | Kernel version too old (fix in v5.6-rc1, pre-v5.15); kSTEP supports v5.15+ only |
| `7226017ad37a` | uclamp | Missing effective uclamp propagation on new cgroup creation | Kernel version too old (fix in v5.6-rc1, pre-v5.15); kSTEP supports v5.15+ only |
