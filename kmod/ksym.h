#include <linux/types.h>

// Define function symbols
// Format: X(ret_type, func_name, args)
#define KSYM_FUNC_LIST                                                         \
  X(void, tick_sched_timer_dying, (int cpu))                                   \
  X(void, sched_tick, (void))                                                  \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))                                         \
  X(void, tick_setup_sched_timer, (bool hrtimer))                              \
  X(u64, sched_clock, (void))                                                  \
  X(int, workqueue_offline_cpu, (int cpu))

// Define variable symbols
// Format: X(type, var_name)
#define KSYM_VAR_LIST                                                          \
  X(struct rq, runqueues)                                                      \
  X(void, cd)                                                                  \
  X(u64, __sched_clock_offset)                                                 \
  X(unsigned int, sysctl_sched_migration_cost)

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
