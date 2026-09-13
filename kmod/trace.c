#include <linux/ftrace.h>

#include "internal.h"

// Every hook shares one ftrace_ops, registered once (see kstep_trace_init), and filters are set
// by address: ftrace_set_filter() by name walks every traced function through kallsyms, 0.1 to
// 0.3 s per hook under emulation, where ftrace_set_filter_ip() takes a few ms. The dispatcher
// tells the hooks apart by the traced location.
struct kstep_hook {
  const char *name;
  ftrace_func_t func;
  unsigned long ip; // what ftrace reports for this function (its patched call site)
};
static struct kstep_hook kstep_hooks[4];
static int kstep_nhooks;

static void kstep_dispatch(unsigned long ip, unsigned long parent_ip, struct ftrace_ops *op,
                           struct ftrace_regs *fregs) {
  for (int i = 0; i < kstep_nhooks; i++)
    if (kstep_hooks[i].ip == ip)
      return kstep_hooks[i].func(ip, parent_ip, op, fregs);
}

static struct ftrace_ops kstep_ftrace_ops = {
    .func = kstep_dispatch,
    .flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION,
};

static void kstep_hook(const char *name, ftrace_func_t func) {
  KSYM_IMPORT(ftrace_location);
  unsigned long addr = (unsigned long)kstep_ksym_lookup(name);
  unsigned long ip = addr ? KSYM_ftrace_location(addr) : 0;

  if (!ip)
    panic("Cannot trace %s", name);
  if (ftrace_set_filter_ip(&kstep_ftrace_ops, addr, 0, 0))
    panic("Failed to set filter for %s", name);
  kstep_hooks[kstep_nhooks++] = (struct kstep_hook){name, func, ip};
}

// Callback at sched_balance_rq(int this_cpu, struct rq *this_rq,
//     struct sched_domain *sd, enum cpu_idle_type idle, int
//     *continue_balancing)
// https://github.com/torvalds/linux/commit/4c3e509ea9f249458e8692f8298cceac73105948
// load_balance -> sched_balance_rq
static DEFINE_PER_CPU(int, lb_dst_cpu);
static DEFINE_PER_CPU(struct sched_domain *, lb_sd);
static void on_sched_balance_enter(unsigned long ip, unsigned long parent_ip,
                                   struct ftrace_ops *op,
                                   struct ftrace_regs *fregs) {
  int this_cpu = (int)regs_get_kernel_argument((void *)fregs, 0);
  struct sched_domain *sd =
      (struct sched_domain *)regs_get_kernel_argument((void *)fregs, 2);
  if (this_cpu == 0 || kstep_jiffies_get() == 0)
    return;
  __this_cpu_write(lb_dst_cpu, this_cpu);
  __this_cpu_write(lb_sd, sd);
  if (kstep_driver->on_sched_balance_begin)
    kstep_driver->on_sched_balance_begin(this_cpu, sd);
}

// Callback at sched_balance_find_src_group(struct lb_env *env)
// Called after should_we_balance(struct lb_env *env) returns true
// https://github.com/torvalds/linux/commit/82cf921432fc184adbbb9c1bced182564876ec5e
// find_busiest_group -> sched_balance_find_src_group
static void on_sched_balance_selected(unsigned long ip, unsigned long parent_ip,
                                      struct ftrace_ops *op,
                                      struct ftrace_regs *fregs) {
  int cpu = __this_cpu_read(lb_dst_cpu);
  struct sched_domain *sd = __this_cpu_read(lb_sd);
  if (cpu == 0 || kstep_jiffies_get() == 0)
    return;
  if (kstep_driver->on_sched_balance_selected)
    kstep_driver->on_sched_balance_selected(cpu, sd);
}

// Callback at set_task_cpu(struct task_struct *p, unsigned int new_cpu): every
// migration, whether by the load balancer or by wakeup placement.
static void on_set_task_cpu(unsigned long ip, unsigned long parent_ip,
                            struct ftrace_ops *op, struct ftrace_regs *fregs) {
  struct task_struct *p =
      (struct task_struct *)regs_get_kernel_argument((void *)fregs, 0);
  int new_cpu = (int)regs_get_kernel_argument((void *)fregs, 1);
  if (kstep_jiffies_get() == 0 || task_cpu(p) == new_cpu)
    return;
  kstep_driver->on_task_migrate(p, task_cpu(p), new_cpu);
}

// Callback at init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
//     struct sched_entity *se, int cpu, struct sched_entity *parent)
// Also sets min_vruntime for new task groups
static void on_sched_group_alloc(unsigned long ip, unsigned long parent_ip,
                                 struct ftrace_ops *op,
                                 struct ftrace_regs *fregs) {
  struct task_group *tg = (void *)regs_get_kernel_argument((void *)fregs, 0);
  struct cfs_rq *cfs_rq = (void *)regs_get_kernel_argument((void *)fregs, 1);
  int cpu = (int)regs_get_kernel_argument((void *)fregs, 3);

// https://github.com/torvalds/linux/commit/79f3f9bedd149ea438aaeb0fb6a083637affe205
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  cfs_rq->zero_vruntime = INIT_TIME_NS;
#else
  cfs_rq->min_vruntime = INIT_TIME_NS;
#endif
  TRACE_INFO("Set min vruntime to %llu ns on cpu %d", INIT_TIME_NS, cpu);

  if (kstep_driver->on_sched_group_alloc)
    kstep_driver->on_sched_group_alloc(tg, cpu);
}

// Hook what this driver needs, all at once. The balance and migrate hooks are silent until the
// mocked clock runs (jiffies 0 during setup), so hooking before setup changes nothing.
void kstep_trace_init(void) {
  kstep_hook("init_tg_cfs_entry", on_sched_group_alloc);
  if (kstep_driver->on_sched_balance_begin || kstep_driver->on_sched_balance_selected)
    kstep_hook(kstep_ksym_lookup("sched_balance_rq") ? "sched_balance_rq" : "load_balance", on_sched_balance_enter);
  if (kstep_driver->on_sched_balance_selected)
    kstep_hook(kstep_ksym_lookup("sched_balance_find_src_group") ? "sched_balance_find_src_group" : "find_busiest_group",
               on_sched_balance_selected);
  if (kstep_driver->on_task_migrate)
    kstep_hook("set_task_cpu", on_set_task_cpu);
  if (register_ftrace_function(&kstep_ftrace_ops))
    panic("Failed to register the ftrace hooks");
  for (int i = 0; i < kstep_nhooks; i++)
    TRACE_INFO("Traced %s", kstep_hooks[i].name);
}
