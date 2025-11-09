#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/version.h>

#include "internal.h"
#include "ksym.h"
#include "logging.h"

// Module parameters
static char *trace_funcs[32] = {};
static int trace_func_count = 0;
module_param_array(trace_funcs, charp, &trace_func_count, 0644);
MODULE_PARM_DESC(trace_funcs, "Function names to trace");

// https://github.com/torvalds/linux/commit/94d095ffa0e16bb7f161a2b73bbe5c2795d499a8
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
#define ftrace_override_function_with_return(fregs)                            \
  ksym.override_function_with_return(arch_ftrace_get_regs(fregs))
#define ftrace_regs_get_argument(fregs, n)                                     \
  regs_get_kernel_argument(arch_ftrace_get_regs(fregs), n)
#endif

// https://github.com/torvalds/linux/commit/e4cf33ca48128d580e25ebe779b7ba7b4b4cf733
#ifdef arch_ftrace_regs
#undef ftrace_override_function_with_return
#define ftrace_override_function_with_return(fregs)                            \
  ksym.override_function_with_return(&arch_ftrace_regs(fregs)->regs)
#else
#undef ftrace_override_function_with_return
#define ftrace_override_function_with_return(fregs) do { } while(0)
#endif

// do not call the original function, and directly return to the caller
static void noop_cb(unsigned long ip, unsigned long parent_ip,
                    struct ftrace_ops *op, struct ftrace_regs *fregs) {
  ftrace_override_function_with_return(fregs);
}

static void sched_tick_cb(unsigned long ip, unsigned long parent_ip,
                          struct ftrace_ops *op, struct ftrace_regs *fregs) {
  int cpu = smp_processor_id();
  if (cpu == 0)
    return;
  TRACE_INFO("sched_tick called on CPU %d", cpu);
}

static void update_rq_clock_cb(unsigned long ip, unsigned long parent_ip,
                               struct ftrace_ops *op,
                               struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;

  struct rq *rq = (void *)ftrace_regs_get_argument(fregs, 0);
  u64 clock = sched_clock();
  s64 delta = (s64)clock - rq->clock;
  TRACE_INFO("update_rq_clock called on CPU %d, clock=%llu, rq->clock=%llu, "
             "delta=%lld",
             smp_processor_id(), clock, rq->clock, delta);
}

static void sched_balance_domains_cb(unsigned long ip, unsigned long parent_ip,
                                     struct ftrace_ops *op,
                                     struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;
  struct rq *rq = (void *)ftrace_regs_get_argument(fregs, 0);
  TRACE_INFO("sched_balance_domains called on CPU %d: next_balance=%lu, "
             "jiffies=%lu, diff=%ld",
             rq->cpu, rq->next_balance, jiffies, rq->next_balance - jiffies);
}

static void sched_balance_rq_cb(unsigned long ip, unsigned long parent_ip,
                                struct ftrace_ops *op,
                                struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;
  int cpu = ftrace_regs_get_argument(fregs, 0);
  struct rq *rq = (void *)ftrace_regs_get_argument(fregs, 1);
  struct sched_domain *sd = (void *)ftrace_regs_get_argument(fregs, 2);

  TRACE_INFO(
      "sched_balance_rq called on CPU %d: sd->name=%s, sd->last_balance=%ld",
      cpu, sd->name, sd->last_balance);
}

struct trace_func_info {
  // input parameters
  ftrace_func_t callback;
  char *name;
  // internal state
  bool enabled;
  struct ftrace_ops op;
};

static struct trace_func_info trace_func_infos[] = {
    {.callback = &sched_tick_cb, .name = "sched_tick"},
    {.callback = &sched_tick_cb, .name = "scheduler_tick"},
    {.callback = &update_rq_clock_cb, .name = "update_rq_clock"},
    {.callback = &sched_balance_domains_cb, .name = "sched_balance_domains"},
    {.callback = &sched_balance_rq_cb, .name = "sched_balance_rq"},
};

static void trace_func_enable(struct trace_func_info *info) {
  info->op.func = info->callback;
  info->op.flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED;
  ftrace_set_filter(&info->op, info->name, strlen(info->name), 1);
  if (register_ftrace_function(&info->op)) {
    TRACE_ERR("Failed to trace %s", info->name);
  } else {
    TRACE_INFO("Tracing %s", info->name);
    info->enabled = true;
  }
}

static void trace_func_disable(struct trace_func_info *info) {
  if (!info->enabled)
    return;
  info->enabled = false;
  unregister_ftrace_function(&info->op);
}

static struct trace_func_info *trace_func_find(char *name) {
  for (int i = 0; i < ARRAY_SIZE(trace_func_infos); i++) {
    struct trace_func_info *info = &trace_func_infos[i];
    if (strcmp(name, info->name) == 0) {
      return info;
    }
  }
  return NULL;
}

void kstep_make_function_noop(char *name) {
  struct trace_func_info *info =
      kcalloc(1, sizeof(struct trace_func_info), GFP_KERNEL);
  info->callback = &noop_cb;
  info->name = name;
  trace_func_enable(info);
}

int kstep_trace_init(void) {
  for (int i = 0; i < trace_func_count; i++) {
    char *name = trace_funcs[i];
    struct trace_func_info *info = trace_func_find(name);
    if (info) {
      trace_func_enable(info);
    } else {
      TRACE_ERR("Function %s not found", name);
    }
  }

  TRACE_INFO("Scheduler trace initialized");
  return 0;
}

void kstep_trace_exit(void) {
  for (int i = 0; i < ARRAY_SIZE(trace_func_infos); i++) {
    struct trace_func_info *info = &trace_func_infos[i];
    trace_func_disable(info);
  }
  TRACE_INFO("Scheduler trace uninitialized");
}
