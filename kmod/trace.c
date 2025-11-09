#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/sched/clock.h>
#include <linux/version.h>

#include "internal.h"
#include "ksym.h"
#include "logging.h"

struct trace_func_info {
  struct list_head list;
  struct ftrace_ops op;
};

static LIST_HEAD(trace_func_list);

static void kstep_trace_function(char *name, ftrace_func_t callback) {
  struct trace_func_info *info =
      kcalloc(1, sizeof(struct trace_func_info), GFP_KERNEL);
  info->op.func = callback;
  info->op.flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED |
                   FTRACE_OPS_FL_DYNAMIC | FTRACE_OPS_FL_RECURSION;
  ftrace_set_filter(&info->op, name, strlen(name), 1);
  if (register_ftrace_function(&info->op)) {
    TRACE_ERR("Failed to trace %s", name);
    kfree(info);
  } else {
    TRACE_INFO("Tracing %s", name);
    list_add(&info->list, &trace_func_list);
  }
}

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

void kstep_make_function_noop(char *name) {
  kstep_trace_function(name, &noop_cb);
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

void kstep_trace_rq_clock(void) {
  kstep_trace_function("update_rq_clock", &update_rq_clock_cb);
}

int kstep_trace_init(void) {
  // kstep_trace_rq_clock();
  TRACE_INFO("Scheduler trace initialized");
  return 0;
}

void kstep_trace_exit(void) {
  TRACE_INFO("Scheduler trace uninitialized");
  struct trace_func_info *info;
  list_for_each_entry(info, &trace_func_list, list) {
    unregister_ftrace_function(&info->op);
    kfree(info);
  }
}
