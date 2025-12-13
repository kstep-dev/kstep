#include <linux/tracepoint.h>

#include "kstep.h"

void kstep_print_rq_stats(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    int h_nr_runnable_val = 0, h_nr_queued_val = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
    h_nr_runnable_val = rq->cfs.h_nr_runnable;
    h_nr_queued_val = rq->cfs.h_nr_queued;
#endif

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    u64 avg_load = rq->cfs.avg_load;
    u64 avg_vruntime = ksym.avg_vruntime(&rq->cfs) - INIT_TIME_NS;
#else
    u64 avg_load = 0;
    u64 avg_vruntime = 0;
#endif

    pr_info("print_rq_stats: CPU %d running=%d, queued=%d, switches=%3lld, "
            "avg_load=%lld, "
            "avg_util=%lu, min_vruntime=%lld, avg_vruntime=%lld\n",
            cpu, rq->nr_running - (h_nr_queued_val - h_nr_runnable_val),
            rq->nr_running, rq->nr_switches, avg_load,
            rq->avg_rt.util_avg + rq->cfs.avg.util_avg + rq->avg_dl.util_avg,
            rq->cfs.min_vruntime - INIT_TIME_NS, avg_vruntime);
  }
}

void kstep_print_tasks(void) {
  struct task_struct *p;

  pr_info("\t%3s %c%s %5s %5s %12s %12s %9s\n", "CPU", ' ', "S", "PID", "PPID",
          "vruntime", "sum-exec", "switches");
  pr_info("\t-------------------------------------------------------------\n");

  for_each_process(p) {
    if (task_cpu(p) == 0 || kstep_is_sys_kthread(p))
      continue;
    pr_info("\tprint_tasks: %3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu\n",
            task_cpu(p), p->on_cpu ? '>' : ' ', task_state_to_char(p),
            task_pid_nr(p), task_ppid_nr(p), p->se.vruntime,
            p->se.sum_exec_runtime, p->nvcsw, p->nivcsw);
  }
}

void kstep_print_nr_running(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    pr_info("print_nr_running: %d %d\n", cpu, rq->nr_running);
  }
}

static DEFINE_PER_CPU(ktime_t, sched_softirq_starttime) = 0;

static void sched_softirq_entry(void *ignore, unsigned int vec_nr) {
  if (vec_nr != SCHED_SOFTIRQ || smp_processor_id() == 0)
    return;
  this_cpu_write(sched_softirq_starttime, ktime_get());
}

static void sched_softirq_exit(void *ignore, unsigned int vec_nr) {
  if (vec_nr != SCHED_SOFTIRQ || smp_processor_id() == 0)
    return;

  ktime_t endtime = ktime_get();
  ktime_t starttime = this_cpu_read(sched_softirq_starttime);
  if (!starttime)
    panic("sched_softirq_starttime is not set");
  this_cpu_write(sched_softirq_starttime, 0);
  ktime_t duration = ktime_sub(endtime, starttime);
  pr_info("run_rebalance_domains on CPU %d, latency: %lld ns\n",
          smp_processor_id(), ktime_to_ns(duration));
}

void kstep_trace_sched_softirq(void) {
  if (tracepoint_probe_register((void *)ksym.__tracepoint_softirq_entry,
                                sched_softirq_entry, NULL))
    panic("Failed to register softirq_entry tracepoint");

  if (tracepoint_probe_register((void *)ksym.__tracepoint_softirq_exit,
                                sched_softirq_exit, NULL))
    panic("Failed to register softirq_exit tracepoint");

  TRACE_INFO("Tracing sched_softirq latency");
}

struct lb_env {
  struct sched_domain *sd;
  struct rq *src_rq;
  int src_cpu;
  int dst_cpu;
  struct rq *dst_rq;
  // other fields are not needed
};

static void load_balance_enter(unsigned long ip, unsigned long parent_ip,
                               struct ftrace_ops *op,
                               struct ftrace_regs *fregs) {
  struct lb_env *env = (void *)regs_get_kernel_argument((void *)fregs, 0);
  if (env->dst_cpu >= 4 && env->dst_cpu <= 7) {
    pr_info("LB %d %d %d %d %d\n", env->dst_cpu, env->sd->span_weight,
            env->sd->groups->group_weight,
            cpumask_first(sched_domain_span(env->sd)),
            cpumask_last(sched_domain_span(env->sd)));
  }
}

struct ftrace_ops load_balance_enter_op = {
    .func = load_balance_enter,
    .flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED | FTRACE_OPS_FL_RECURSION,
};

void kstep_trace_load_balance(void) {
  // We use `sched_balance_find_src_group`/`find_busiest_group` as a proxy to
  // trace `sched_balance_rq`/`load_balance` when `should_we_balance` is true.

// https://github.com/torvalds/linux/commit/82cf921432fc184adbbb9c1bced182564876ec5e
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
  char *name = "sched_balance_find_src_group";
#else
  char *name = "find_busiest_group";
#endif

  if (ftrace_set_filter(&load_balance_enter_op, name, strlen(name), 1))
    panic("Failed to set filter for %s", name);

  if (register_ftrace_function(&load_balance_enter_op))
    panic("Failed to register ftrace function for %s", name);

  TRACE_INFO("Traced %s for load balancing", name);
}
