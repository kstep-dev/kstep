#include <linux/ftrace.h>

#include "kstep.h"

static void kstep_trace_function(char *name, ftrace_func_t callback) {
  struct ftrace_ops *op = kcalloc(1, sizeof(struct ftrace_ops), GFP_KERNEL);

  op->func = callback;
  op->flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION;

  if (ftrace_set_filter(op, name, strlen(name), 1)) {
    TRACE_ERR("Failed to set filter for %s", name);
    kfree(op);
    return;
  }

  if (register_ftrace_function(op)) {
    TRACE_ERR("Failed to trace %s", name);
    kfree(op);
    return;
  }
}

//
// Trace load balancing
//

static void find_busiest_group_cb(unsigned long ip, unsigned long parent_ip,
                                  struct ftrace_ops *op,
                                  struct ftrace_regs *fregs) {
  struct lb_env {
    struct sched_domain *sd;

    struct rq *src_rq;
    int src_cpu;

    int dst_cpu;
    struct rq *dst_rq;

    // other fields are not needed
  };

  struct lb_env *env = (void *)regs_get_kernel_argument((void *)fregs, 0);
  if (env->dst_cpu >= 4 && env->dst_cpu <= 7) {
    printk("LB %d %d %d %d %d\n", env->dst_cpu, env->sd->span_weight,
           env->sd->groups->group_weight,
           cpumask_first(sched_domain_span(env->sd)),
           cpumask_last(sched_domain_span(env->sd)));
  }
}

void kstep_trace_lb(void) {
  // Instead of tracing in the middle of load_balance (in between
  // should_we_balance and find_busiest_group), we trace find_busiest_group
  // instead to check if the load balance is happening.
  # if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    kstep_trace_function("sched_balance_find_src_group", &find_busiest_group_cb);
  #else
    kstep_trace_function("find_busiest_group", &find_busiest_group_cb);
  #endif
  TRACE_INFO("Traced find_busiest_group for load balancing");
}
