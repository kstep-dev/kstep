#include <linux/version.h>

// Use `KSYM_AUTO` for variables or functions with existing declarations
// available in the header file, so their type can be inferred.
#define KSYM_AUTO(name) KSYM_VAR(typeof(name), name)

// Declares function `ksym.func_name`
#ifndef KSYM_FUNC
#define KSYM_FUNC(ret_type, func_name, arg...)
#endif

// Declares `ksym.name` as a *pointer* to the variable
#ifndef KSYM_VAR
#define KSYM_VAR(type, name)
#endif

// tick.c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
KSYM_AUTO(sched_tick)
#else
KSYM_AUTO(scheduler_tick)
#endif
#if defined(CONFIG_PARAVIRT) && defined(CONFIG_X86_64)
KSYM_AUTO(__sched_clock_offset)
KSYM_AUTO(paravirt_set_sched_clock)
#elif defined(CONFIG_GENERIC_SCHED_CLOCK)
KSYM_VAR(void, cd)
#endif
KSYM_AUTO(tick_get_tick_sched)
KSYM_AUTO(tick_do_timer_cpu)
KSYM_AUTO(tick_next_period)

// isolation.c
KSYM_AUTO(workqueue_offline_cpu)

// reset.c
KSYM_AUTO(update_rq_clock)
KSYM_AUTO(sysctl_sched_migration_cost)
KSYM_VAR(int, distribute_cpu_mask_prev)

// kernel.c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
KSYM_AUTO(entity_eligible)
#endif
KSYM_AUTO(freeze_task)
KSYM_AUTO(pm_freezing)

// output.c
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
KSYM_AUTO(avg_vruntime)
#endif
KSYM_AUTO(effective_cpu_util)
KSYM_VAR(void *, __tracepoint_softirq_entry)
KSYM_VAR(void *, __tracepoint_softirq_exit)

// topo.c
KSYM_AUTO(rebuild_sched_domains)
#if !defined(CONFIG_GENERIC_ARCH_TOPOLOGY) &&                                  \
    LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
KSYM_AUTO(arch_enable_hybrid_capacity_scale)
KSYM_AUTO(arch_set_cpu_capacity)
#endif
KSYM_AUTO(arch_freq_scale)
KSYM_VAR(struct sched_domain_topology_level *, sched_domain_topology)
#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
KSYM_AUTO(update_topology)
#else
KSYM_AUTO(x86_topology_update)
#endif

// shared
KSYM_AUTO(runqueues)
#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(ksym.runqueues, (cpu)))
#undef this_rq
#define this_rq() this_cpu_ptr(ksym.runqueues)
#undef raw_rq
#define raw_rq() raw_cpu_ptr(ksym.runqueues)

// misc
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
KSYM_FUNC(void, dequeue_entities, struct cfs_rq *cfs_rq,
          struct sched_entity *se, int flags)
#endif

#undef KSYM_AUTO
#undef KSYM_VAR
#undef KSYM_FUNC
