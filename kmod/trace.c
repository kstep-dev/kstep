#include <linux/ftrace.h>
#include <linux/sched/clock.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
#include <linux/fprobe.h>
#endif

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
// Set min vruntime for newly created cfs_rq
//

static void init_tg_cfs_entry_cb(unsigned long ip, unsigned long parent_ip,
                                 struct ftrace_ops *op,
                                 struct ftrace_regs *fregs) {
  struct cfs_rq *cfs_rq = (void *)regs_get_kernel_argument((void *)fregs, 1);
  cfs_rq->min_vruntime = INIT_TIME_NS;
  TRACE_INFO("Set min vruntime to %llu ns", INIT_TIME_NS);
}

void kstep_patch_min_vruntime(void) {
  kstep_trace_function("init_tg_cfs_entry", &init_tg_cfs_entry_cb);
  TRACE_INFO("Patched init_tg_cfs_entry to set min vruntime");
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
  kstep_trace_function("find_busiest_group", &find_busiest_group_cb);
  TRACE_INFO("Traced find_busiest_group for load balancing");
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)

//
// Trace rebalance duration
//

static DEFINE_PER_CPU(ktime_t, rebalance_domains_starttime);

static int run_rebalance_domains_entry(void) {
  if (smp_processor_id() == 0)
    return 1; // skip exit handler
  this_cpu_write(rebalance_domains_starttime, ktime_get());
  return 0;
}

static void run_rebalance_domains_exit(void) {
  if (smp_processor_id() == 0)
    return;
  ktime_t endtime = ktime_get();
  ktime_t starttime = this_cpu_read(rebalance_domains_starttime);
  ktime_t duration = ktime_sub(endtime, starttime);
  printk(KERN_INFO "run_rebalance_domains on CPU %d, latency: %lld ns\n",
         smp_processor_id(), ktime_to_ns(duration));
}

static struct fprobe fp_rebalance = {
    .entry_handler = (void *)run_rebalance_domains_entry,
    .exit_handler = (void *)run_rebalance_domains_exit,
};

void kstep_trace_rebalance(void) {
// https://github.com/torvalds/linux/commit/70a27d6d1b19392a23bb4a41de7788fbc539f18d
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  const char *name = "sched_balance_softirq";
#else
  const char *name = "run_rebalance_domains";
#endif
  if (register_fprobe(&fp_rebalance, name, NULL) < 0) {
    TRACE_ERR("Failed to register fprobe %s to trace rebalance duration", name);
  } else {
    TRACE_INFO("Traced rebalance duration with fprobe %s", name);
  }
}
#else
void kstep_trace_rebalance(void) {
  TRACE_ERR("Fprobe not supported in this kernel version");
}
#endif
