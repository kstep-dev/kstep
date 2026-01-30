#include <linux/version.h>

#ifndef KSYM_FUNC
// Declares function `ksym.func_name`
#define KSYM_FUNC(ret_type, func_name, arg...)
#endif

#ifndef KSYM_VAR
// Declares `ksym.name` as a *pointer* to the variable
#define KSYM_VAR(type, name)
#endif

// tick.c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
KSYM_FUNC(void, sched_tick, void)
#else
KSYM_FUNC(void, scheduler_tick, void)
#endif
#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
KSYM_VAR(u64, __sched_clock_offset)
KSYM_FUNC(void, paravirt_set_sched_clock, u64 (*func)(void))
#elif defined(CONFIG_GENERIC_SCHED_CLOCK)
KSYM_VAR(void, cd)
#endif
KSYM_FUNC(struct tick_sched *, tick_get_tick_sched, int cpu)
KSYM_VAR(int, tick_do_timer_cpu)
KSYM_VAR(ktime_t, tick_next_period)

// isolation.c
KSYM_FUNC(int, workqueue_offline_cpu, int cpu)

// reset.c
KSYM_FUNC(void, update_rq_clock, struct rq *rq)
KSYM_VAR(unsigned int, sysctl_sched_migration_cost)
KSYM_VAR(int, distribute_cpu_mask_prev)

// kernel.c
KSYM_FUNC(int, entity_eligible, struct cfs_rq *cfs_rq, struct sched_entity *se)
KSYM_FUNC(void, freeze_task, struct task_struct *p)
KSYM_VAR(bool, pm_freezing)

// output.c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
KSYM_FUNC(u64, avg_vruntime, struct cfs_rq *cfs_rq)
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
KSYM_FUNC(unsigned long, effective_cpu_util, int cpu, unsigned long util_cfs,
          unsigned long *min, unsigned long *max)
#else
KSYM_FUNC(unsigned long, effective_cpu_util, int cpu, unsigned long util_cfs,
          unsigned long max, enum cpu_util_type type, struct task_struct *p)
#endif
KSYM_VAR(void *, __tracepoint_softirq_entry)
KSYM_VAR(void *, __tracepoint_softirq_exit)

// topo.c
KSYM_FUNC(void, rebuild_sched_domains, void)
KSYM_FUNC(bool, arch_enable_hybrid_capacity_scale, void)
KSYM_FUNC(void, arch_set_cpu_capacity, int cpu, unsigned long cap,
          unsigned long max_cap, unsigned long cap_freq,
          unsigned long base_freq)
KSYM_VAR(unsigned long, arch_freq_scale)
KSYM_VAR(struct sched_domain_topology_level *, sched_domain_topology)
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
KSYM_VAR(int, update_topology)
#else
KSYM_VAR(bool, x86_topology_update)
#endif

// shared
KSYM_VAR(struct rq, runqueues)
#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(ksym.runqueues, (cpu)))
#undef this_rq
#define this_rq() this_cpu_ptr(ksym.runqueues)
#undef raw_rq
#define raw_rq() raw_cpu_ptr(ksym.runqueues)

// misc
KSYM_FUNC(void, dequeue_entities, struct cfs_rq *cfs_rq,
          struct sched_entity *se, int flags)

#undef KSYM_FUNC
#undef KSYM_VAR
