#include <linux/mmu_context.h>
#include <linux/sched/cputime.h>
#include <linux/types.h>

// private headers
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
#include <kernel/sched/sched.h>
#include <kernel/time/tick-sched.h>
#pragma GCC diagnostic pop

// Define function symbols
// X(ret_type, func_name, args) declares function `ksym.func_name`
#define KSYM_FUNC_LIST                                                         \
  X(void, sched_tick, (void))                                                  \
  X(void, scheduler_tick, (void))                                              \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))                                         \
  X(void, tick_setup_sched_timer, (bool hrtimer))                              \
  X(int, workqueue_offline_cpu, (int cpu))                                     \
  X(void, update_rq_clock, (struct rq * rq))                                   \
  X(int, entity_eligible, (struct cfs_rq * cfs_rq, struct sched_entity * se))  \
  X(void, signal_wake_up_state, (struct task_struct * t, int state))           \
  X(int, try_to_wake_up,                                                       \
    (struct task_struct * p, unsigned int state, int wake_flags))              \
  X(void, freeze_task, (struct task_struct * p))                               \
  X(void, dequeue_entities,                                                    \
    (struct cfs_rq * cfs_rq, struct sched_entity * se, int flags))             \
  X(u64, avg_vruntime, (struct cfs_rq * cfs_rq))                               \
  X(struct tick_sched *, tick_get_tick_sched, (int cpu))                       \
  X(void, override_function_with_return, (struct pt_regs * regs))              \
  X(void, rebuild_sched_domains, (void))                                       \
  X(bool, arch_enable_hybrid_capacity_scale, (void))                           \
  X(void, arch_set_cpu_capacity,                                               \
    (int cpu, unsigned long cap, unsigned long max_cap,                        \
     unsigned long cap_freq, unsigned long base_freq))

// Define variable symbols
// X(type, var_name) declares `ksym.var_name` as a *pointer* to the variable
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)                                                                  \
  X(u64, __sched_clock_offset)                                                 \
  X(unsigned int, sysctl_sched_migration_cost)                                 \
  X(bool, pm_freezing)                                                         \
  X(unsigned long, arch_freq_scale)                                            \
  X(const struct sched_class, rt_sched_class)                                  \
  X(int, tick_do_timer_cpu)                                                    \
  X(int, distribute_cpu_mask_prev)                                             \
  X(struct sched_domain_topology_level *, sched_domain_topology)               \
  X(int, update_topology)                                                      \
  X(bool, x86_topology_update)

struct ksym_t {
  // Used for dynamic symbol lookup
  void *(*kallsyms_lookup_name)(const char *name);

#define X(ret_type, name, args) ret_type(*name) args;
  KSYM_FUNC_LIST
#undef X

#define X(type, name) type *name;
  KSYM_VAR_LIST
#undef X
};

extern struct ksym_t ksym;

void ksym_init(void);

#undef cpu_rq
#define cpu_rq(cpu) (per_cpu_ptr(ksym.runqueues, (cpu)))

#undef this_rq
#define this_rq() this_cpu_ptr(ksym.runqueues)

#undef raw_rq
#define raw_rq() raw_cpu_ptr(ksym.runqueues)
