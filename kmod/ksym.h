#include <linux/version.h>

// Define function symbols
// KSYM_FUNC(ret_type, func_name, arg...) declares function `ksym.func_name`
#ifndef KSYM_FUNC
#define KSYM_FUNC(ret_type, func_name, arg...)
#endif

// Define variable symbols
// KSYM_VAR(type, name) declares `ksym.name` as a *pointer* to the variable
#ifndef KSYM_VAR
#define KSYM_VAR(type, name)
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
KSYM_FUNC(void, sched_tick, void)
#else
KSYM_FUNC(void, scheduler_tick, void)
#endif
KSYM_FUNC(void, paravirt_set_sched_clock, u64 (*func)(void))
KSYM_FUNC(u64, kvm_sched_clock_read, void)
KSYM_FUNC(void, tick_setup_sched_timer, bool hrtimer)
KSYM_FUNC(int, workqueue_offline_cpu, int cpu)
KSYM_FUNC(void, update_rq_clock, struct rq *rq)
KSYM_FUNC(int, entity_eligible, struct cfs_rq *cfs_rq, struct sched_entity *se)
KSYM_FUNC(int, try_to_wake_up, struct task_struct *p, unsigned int state,
          int wake_flags)
KSYM_FUNC(void, freeze_task, struct task_struct *p)
KSYM_FUNC(void, dequeue_entities, struct cfs_rq *cfs_rq,
          struct sched_entity *se, int flags)
KSYM_FUNC(u64, avg_vruntime, struct cfs_rq *cfs_rq)
KSYM_FUNC(struct tick_sched *, tick_get_tick_sched, int cpu)
KSYM_FUNC(void, rebuild_sched_domains, void)
KSYM_FUNC(bool, arch_enable_hybrid_capacity_scale, void)
KSYM_FUNC(void, arch_set_cpu_capacity, int cpu, unsigned long cap,
          unsigned long max_cap, unsigned long cap_freq,
          unsigned long base_freq)

KSYM_VAR(struct rq, runqueues)
#ifdef CONFIG_GENERIC_SCHED_CLOCK
KSYM_VAR(void, cd)
#endif
KSYM_VAR(u64, __sched_clock_offset)
KSYM_VAR(unsigned int, sysctl_sched_migration_cost)
KSYM_VAR(bool, pm_freezing)
KSYM_VAR(unsigned long, arch_freq_scale)
KSYM_VAR(int, tick_do_timer_cpu)
KSYM_VAR(ktime_t, tick_next_period)
KSYM_VAR(int, distribute_cpu_mask_prev)
KSYM_VAR(struct sched_domain_topology_level *, sched_domain_topology)
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
KSYM_VAR(int, update_topology)
#else
KSYM_VAR(bool, x86_topology_update)
#endif
KSYM_VAR(void *, __tracepoint_softirq_entry)
KSYM_VAR(void *, __tracepoint_softirq_exit)

#undef KSYM_FUNC
#undef KSYM_VAR
