#include <linux/ftrace.h>
#include <linux/kallsyms.h>

#include "event.h"
#include "internal.h"

// The kernel side of the events in event.h: each traced function is hooked once and raised to
// everyone registered for it. What is traced follows from who is watching -- registering for an
// event arms the hook behind it, whenever that happens -- so a session traces nothing for an event
// nobody watches.
//
// Every hook shares one ftrace_ops, registered once, and filters are set by address:
// ftrace_set_filter() by name walks every traced function through kallsyms, 0.1 to 0.3 s per hook
// under emulation, where ftrace_set_filter_ip() takes a few ms. The dispatcher tells the hooks
// apart by the traced location.
struct kstep_hook {
  const char *name;
  ftrace_func_t func;
  unsigned long ip; // what ftrace reports for this function (its patched call site)
};
// The traced events' lists: who is called when each is raised (kstep_emit). The events kstep
// raises itself are in event.c.
static struct kstep_event kstep_event_select_task_rq, kstep_event_balance_selected, kstep_event_task_migrate;

static struct kstep_hook kstep_hooks[8];
static int kstep_nhooks; // published after the entry it counts, for the dispatcher on other CPUs

static void kstep_dispatch(unsigned long ip, unsigned long parent_ip, struct ftrace_ops *op,
                           struct ftrace_regs *fregs) {
  int nhooks = smp_load_acquire(&kstep_nhooks);

  for (int i = 0; i < nhooks; i++)
    if (kstep_hooks[i].ip == ip)
      return kstep_hooks[i].func(ip, parent_ip, op, fregs);
}

static struct ftrace_ops kstep_ftrace_ops = {
    .func = kstep_dispatch,
    .flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION,
};

// Trace name, unless it already is. Safe to call after the ops is registered, which is what lets
// an observer that arrives mid-session (the `check` verb enabling a rule) arm what it needs: the
// entry is published before the filter, so a callback can never find the table short of its ip.
static void kstep_hook(const char *name, ftrace_func_t func) {
  KSYM_IMPORT(ftrace_location_range);
  KSYM_IMPORT(kallsyms_lookup_size_offset);
  unsigned long addr, ip, size = 0, offset;

  for (int i = 0; i < kstep_nhooks; i++)
    if (!strcmp(kstep_hooks[i].name, name))
      return;
  if (WARN_ON(kstep_nhooks == ARRAY_SIZE(kstep_hooks)))
    return;

  // The traced ip is not the entry: on arm64 it is the second patchable nop, entry + 4. Before
  // 5.19 ftrace_location() is an exact match, so find the record anywhere in the function.
  addr = (unsigned long)kstep_ksym_lookup(name);
  if (addr && !KSYM_kallsyms_lookup_size_offset(addr, &size, &offset))
    size = 0;
  ip = size ? KSYM_ftrace_location_range(addr, addr + size - 1) : 0;
  if (!ip)
    panic("Cannot trace %s", name);
  kstep_hooks[kstep_nhooks] = (struct kstep_hook){name, func, ip};
  smp_store_release(&kstep_nhooks, kstep_nhooks + 1);
  if (ftrace_set_filter_ip(&kstep_ftrace_ops, ip, 0, 0))
    panic("Failed to set filter for %s", name);
  TRACE_INFO("Traced %s", name);
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
  kstep_emit(balance_selected, kstep_balance_fn, cpu, sd);
}

// Callback at select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags): a wakeup is
// being placed. Hooked at entry, so a rule sees the runqueues as the placement does; what it chose
// is read off the task once the step has settled.
static void on_select_task_rq(unsigned long ip, unsigned long parent_ip,
                              struct ftrace_ops *op, struct ftrace_regs *fregs) {
  struct task_struct *p = (void *)regs_get_kernel_argument((void *)fregs, 0);
  int prev_cpu = (int)regs_get_kernel_argument((void *)fregs, 1);
  int wake_flags = (int)regs_get_kernel_argument((void *)fregs, 2);

  kstep_emit(select_task_rq, kstep_select_task_rq_fn, p, prev_cpu, wake_flags);
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
  kstep_emit(task_migrate, kstep_task_migrate_fn, p, task_cpu(p), new_cpu);
}

// Callback at init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
//     struct sched_entity *se, int cpu, struct sched_entity *parent): seeds a new task group's
// vruntime. Not an event -- nothing watches it; the session needs the seeding itself.
static void on_sched_group_alloc(unsigned long ip, unsigned long parent_ip,
                                 struct ftrace_ops *op,
                                 struct ftrace_regs *fregs) {
  struct cfs_rq *cfs_rq = (void *)regs_get_kernel_argument((void *)fregs, 1);
  int cpu = (int)regs_get_kernel_argument((void *)fregs, 3);

// https://github.com/torvalds/linux/commit/79f3f9bedd149ea438aaeb0fb6a083637affe205
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  cfs_rq->zero_vruntime = 0;
#else
  cfs_rq->min_vruntime = 0;
#endif
  TRACE_INFO("Set min vruntime to 0 on cpu %d", cpu);
}

// Registering on a traced event: the kernel function behind it is traced the first time someone
// asks for it. A hook may be armed long after the ftrace ops was registered -- that is what lets
// `check <name>` start watching mid-session -- and kstep_hook() is written for that.
//
// The balance and migrate hooks are silent until the mocked clock runs (jiffies 0 during setup),
// so arming before setup changes nothing.
static const char *sym(const char *name, const char *old) { return kstep_ksym_lookup(name) ? name : old; }

void kstep_on_select_task_rq(kstep_select_task_rq_fn fn) {
  if (kstep_event_add(&kstep_event_select_task_rq, fn))
    kstep_hook("select_task_rq_fair", on_select_task_rq);
}

void kstep_on_balance_selected(kstep_balance_fn fn) {
  if (kstep_event_add(&kstep_event_balance_selected, fn)) {
    kstep_hook(sym("sched_balance_rq", "load_balance"), on_sched_balance_enter); // stashes what it reports
    kstep_hook(sym("sched_balance_find_src_group", "find_busiest_group"), on_sched_balance_selected);
  }
}

void kstep_on_task_migrate(kstep_task_migrate_fn fn) {
  if (kstep_event_add(&kstep_event_task_migrate, fn))
    kstep_hook("set_task_cpu", on_set_task_cpu);
}

// init_tg_cfs_entry is traced from the start (below), so this only joins the list.
// init_tg_cfs_entry is traced unconditionally: its handler seeds a new group's vruntime, which the
// session needs whether or not anyone watches the event.
void kstep_trace_init(void) {
  kstep_hook("init_tg_cfs_entry", on_sched_group_alloc);
  if (register_ftrace_function(&kstep_ftrace_ops))
    panic("Failed to register the ftrace hooks");
}
