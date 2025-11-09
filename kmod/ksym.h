#include <linux/mmu_context.h>
#include <linux/sched/cputime.h>
#include <linux/types.h>

// private headers
#include <kernel/sched/sched.h>
#include <kernel/time/tick-sched.h>

// Define function symbols
// Format: X(ret_type, func_name, args)
#define KSYM_FUNC_LIST                                                         \
  X(void, tick_sched_timer_dying, (int cpu))                                   \
  X(void, sched_tick, (void))                                                  \
  X(void, scheduler_tick, (void))                                              \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))                                         \
  X(void, tick_setup_sched_timer, (bool hrtimer))                              \
  X(u64, sched_clock, (void))                                                  \
  X(int, workqueue_offline_cpu, (int cpu))                                     \
  X(void, update_rq_clock, (struct rq * rq))                                   \
  X(int, entity_eligible, (struct cfs_rq * cfs_rq, struct sched_entity * se))  \
  X(void, signal_wake_up_state, (struct task_struct * t, int state))           \
  X(int, try_to_wake_up,                                                       \
    (struct task_struct * p, unsigned int state, int wake_flags))              \
  X(void, sched_yield, (void))                                                 \
  X(void, freeze_task, (struct task_struct * p))                               \
  X(void, tick_offline_cpu, (unsigned int cpu))                                \
  X(void, dequeue_entities,                                                    \
    (struct cfs_rq * cfs_rq, struct sched_entity * se, int flags))             \
  X(u64, avg_vruntime, (struct cfs_rq * cfs_rq))                               \
  X(struct tick_sched *, tick_get_tick_sched, (int cpu))

// Define variable symbols
// Format: X(type, var_name)
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)                                                                  \
  X(u64, __sched_clock_offset)                                                 \
  X(unsigned int, sysctl_sched_migration_cost)                                 \
  X(bool, pm_freezing)                                                         \
  X(unsigned long, arch_freq_scale)                                            \
  X(const struct sched_class, rt_sched_class)

    struct ksym_t {
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
