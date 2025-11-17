#include <linux/ftrace.h>
#include <linux/kernel.h>
#include <linux/percpu.h>
#include <linux/sched/clock.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 18, 0)
#include <linux/fprobe.h>
#endif

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
  if (info == NULL) {
    TRACE_ERR("Failed to allocate memory for trace function %s", name);
    return;
  }

  info->op.func = callback;
  info->op.flags =
      FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION;

  if (ftrace_set_filter(&info->op, name, strlen(name), 1)) {
    TRACE_ERR("Failed to set filter for %s", name);
    kfree(info);
    return;
  }

  if (register_ftrace_function(&info->op)) {
    TRACE_ERR("Failed to trace %s", name);
    kfree(info);
    return;
  }

  list_add(&info->list, &trace_func_list);
}

// do not call the original function, and directly return to the caller
static void noop_cb(unsigned long ip, unsigned long parent_ip,
                    struct ftrace_ops *op, struct ftrace_regs *fregs) {
  ksym.override_function_with_return((void *)fregs);
}

void kstep_patch_func_noop(char *name) {
  kstep_trace_function(name, &noop_cb);
  TRACE_INFO("Patched %s to noop", name);
}

//
// Trace update_rq_clock
//

static void update_rq_clock_cb(unsigned long ip, unsigned long parent_ip,
                               struct ftrace_ops *op,
                               struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;

  struct rq *rq = (void *)regs_get_kernel_argument((void *)fregs, 0);
  u64 clock = sched_clock();
  s64 delta = (s64)clock - rq->clock;
  TRACE_INFO("update_rq_clock called on CPU %d, clock=%llu, rq->clock=%llu, "
             "delta=%lld",
             smp_processor_id(), clock, rq->clock, delta);
}

void kstep_trace_rq_clock(void) {
  kstep_trace_function("update_rq_clock", &update_rq_clock_cb);
  TRACE_INFO("Traced update_rq_clock");
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

static void run_rebalance_domains_entry(void) {
  if (smp_processor_id() == 0)
    return;
  this_cpu_write(rebalance_domains_starttime, ktime_get());
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
  if (register_fprobe(&fp_rebalance, "run_rebalance_domains", NULL) < 0) {
    TRACE_ERR("Failed to register fprobe for run_rebalance_domains");
  } else {
    TRACE_INFO("Traced rebalance duration");
  }
}
#else
void kstep_trace_rebalance(void) {
  TRACE_INFO("Fprobe not supported in this kernel version");
}
#endif

int kstep_trace_init(void) {
  // kstep_trace_rq_clock();
  kstep_patch_min_vruntime();
  TRACE_INFO("Scheduler trace initialized");
  return 0;
}

void kstep_trace_exit(void) {
  struct trace_func_info *info, *tmp;
  list_for_each_entry_safe(info, tmp, &trace_func_list, list) {
    unregister_ftrace_function(&info->op);
    list_del(&info->list);
    kfree(info);
  }
  TRACE_INFO("Scheduler trace uninitialized");
}
