#include <linux/ftrace.h>

#include "internal.h"

// FTRACE_OPS_FL_RECURSION was renamed from FTRACE_OPS_FL_RECURSION_SAFE in 5.11
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define FTRACE_OPS_FL_RECURSION FTRACE_OPS_FL_RECURSION_SAFE
#endif

// Before 5.11, ftrace callbacks use struct pt_regs instead of struct ftrace_regs
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define KSTEP_FTRACE_REGS struct pt_regs
#define KSTEP_FTRACE_REGS_TO_PT(fregs) (fregs)
#else
#define KSTEP_FTRACE_REGS struct ftrace_regs
#define KSTEP_FTRACE_REGS_TO_PT(fregs) ((void *)(fregs))
#endif

#define KSTEP_TRACE_FUNC(name, callback)                                       \
  do {                                                                         \
    static struct ftrace_ops callback##_op = {                                 \
        .func = callback,                                                      \
        .flags =                                                               \
            FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION,    \
    };                                                                         \
    if (ftrace_set_filter(&callback##_op, name, strlen(name), 1))              \
      panic("Failed to set filter for %s", name);                              \
    if (register_ftrace_function(&callback##_op))                              \
      panic("Failed to register ftrace function for %s", name);                \
    TRACE_INFO("Traced %s with %s", name, #callback);                          \
  } while (0)

// Callback at sched_balance_rq(int this_cpu, struct rq *this_rq,
//     struct sched_domain *sd, enum cpu_idle_type idle, int
//     *continue_balancing)
// https://github.com/torvalds/linux/commit/4c3e509ea9f249458e8692f8298cceac73105948
// load_balance -> sched_balance_rq
static DEFINE_PER_CPU(int, lb_dst_cpu);
static DEFINE_PER_CPU(struct sched_domain *, lb_sd);
static void on_sched_balance_enter(unsigned long ip, unsigned long parent_ip,
                                   struct ftrace_ops *op,
                                   KSTEP_FTRACE_REGS *fregs) {
  int this_cpu = (int)regs_get_kernel_argument(KSTEP_FTRACE_REGS_TO_PT(fregs), 0);
  struct sched_domain *sd =
      (struct sched_domain *)regs_get_kernel_argument(KSTEP_FTRACE_REGS_TO_PT(fregs), 2);
  if (this_cpu == 0 || kstep_jiffies_get() == 0)
    return;
  __this_cpu_write(lb_dst_cpu, this_cpu);
  __this_cpu_write(lb_sd, sd);
  if (kstep_driver->on_sched_balance_begin)
    kstep_driver->on_sched_balance_begin(this_cpu, sd);
}

void kstep_trace_sched_balance_begin(void) {
  char *name = kstep_ksym_lookup("sched_balance_rq") ? "sched_balance_rq"
                                                     : "load_balance";
  KSTEP_TRACE_FUNC(name, on_sched_balance_enter);
}

// Callback at sched_balance_find_src_group(struct lb_env *env)
// Called after should_we_balance(struct lb_env *env) returns true
// https://github.com/torvalds/linux/commit/82cf921432fc184adbbb9c1bced182564876ec5e
// find_busiest_group -> sched_balance_find_src_group
static void on_sched_balance_selected(unsigned long ip, unsigned long parent_ip,
                                      struct ftrace_ops *op,
                                      KSTEP_FTRACE_REGS *fregs) {
  int cpu = __this_cpu_read(lb_dst_cpu);
  struct sched_domain *sd = __this_cpu_read(lb_sd);
  if (cpu == 0 || kstep_jiffies_get() == 0)
    return;
  if (kstep_driver->on_sched_balance_selected)
    kstep_driver->on_sched_balance_selected(cpu, sd);
}

void kstep_trace_sched_balance_selected(void) {
  char *name = kstep_ksym_lookup("sched_balance_find_src_group")
                   ? "sched_balance_find_src_group"
                   : "find_busiest_group";
  KSTEP_TRACE_FUNC(name, on_sched_balance_selected);
}

// Callback at init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
//     struct sched_entity *se, int cpu, struct sched_entity *parent)
// Also sets min_vruntime for new task groups
static void on_sched_group_alloc(unsigned long ip, unsigned long parent_ip,
                                 struct ftrace_ops *op,
                                 KSTEP_FTRACE_REGS *fregs) {
  struct task_group *tg = (void *)regs_get_kernel_argument(KSTEP_FTRACE_REGS_TO_PT(fregs), 0);
  struct cfs_rq *cfs_rq = (void *)regs_get_kernel_argument(KSTEP_FTRACE_REGS_TO_PT(fregs), 1);
  int cpu = (int)regs_get_kernel_argument(KSTEP_FTRACE_REGS_TO_PT(fregs), 3);

// https://github.com/torvalds/linux/commit/79f3f9bedd149ea438aaeb0fb6a083637affe205
// Landed in v6.18-rc3 but LINUX_VERSION_CODE stays 6.18.0
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
  cfs_rq->zero_vruntime = INIT_TIME_NS;
#else
  cfs_rq->min_vruntime = INIT_TIME_NS;
#endif
  TRACE_INFO("Set min vruntime to %llu ns on cpu %d", INIT_TIME_NS, cpu);

  if (kstep_driver->on_sched_group_alloc)
    kstep_driver->on_sched_group_alloc(tg, cpu);
}

void kstep_trace_sched_group_alloc(void) {
  KSTEP_TRACE_FUNC("init_tg_cfs_entry", on_sched_group_alloc);
}
