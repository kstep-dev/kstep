#include <linux/ftrace.h>
#include <linux/tracepoint.h>

#include "kstep.h"

#define pr_json_end(key, fmt, expr) pr_cont("\"" #key "\": " fmt, expr)
#define pr_json_cont(key, fmt, expr) pr_json_end(key, fmt ",     ", expr)

static void print_rq(struct rq *rq) {
// https://github.com/torvalds/linux/commit/c2a295bffeaf9461ecba76dc9e4780c898c94f03
// https://github.com/torvalds/linux/commit/7b8a702d943827130cc00ae36075eff5500f86f1
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
  int h_nr_runnable = rq->cfs.h_nr_runnable;
  int h_nr_queued = rq->cfs.h_nr_queued;
#else
  int h_nr_runnable = 0;
  int h_nr_queued = 0;
#endif

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  u64 avg_load = rq->cfs.avg_load;
  u64 avg_vruntime = ksym.avg_vruntime(&rq->cfs) - INIT_TIME_NS;
#else
  u64 avg_load = 0;
  u64 avg_vruntime = 0;
#endif

  u64 avg_util =
      rq->avg_rt.util_avg + rq->cfs.avg.util_avg + rq->avg_dl.util_avg;

  pr_info("rq: {");
  pr_json_cont(cpu, "%d", rq->cpu);
  pr_json_cont(running, "%2d", rq->nr_running - (h_nr_queued - h_nr_runnable));
  pr_json_cont(queued, "%2d", rq->nr_running);
  pr_json_cont(avg_load, "%4llu", avg_load);
  pr_json_cont(avg_util, "%4llu", avg_util);
  pr_json_cont(min_vruntime, "%12lld", rq->cfs.min_vruntime - INIT_TIME_NS);
  pr_json_cont(avg_vruntime, "%12lld", avg_vruntime);
  pr_json_end(timestamp, "%4llu", kstep_tick_count);
  pr_cont("}\n");
}

void kstep_print_rq_stats(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++)
    print_rq(cpu_rq(cpu));
}

static void print_task(struct task_struct *p) {
  pr_info("task: {");
  pr_json_cont(pid, "%d", task_pid_nr(p));
  pr_json_cont(on_cpu, "%5s", p->on_cpu ? "true" : "false");
  pr_json_cont(cpu, "%d", task_cpu(p));
  pr_json_cont(state, "\"%c\"", task_state_to_char(p));
  pr_json_cont(vruntime, "%12lld", p->se.vruntime);
  pr_json_cont(sum_exec, "%12lld", p->se.sum_exec_runtime);
  pr_json_end(timestamp, "%4llu", kstep_tick_count);
  pr_cont("}\n");
}

void kstep_print_tasks(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == 0 || kstep_is_sys_kthread(p))
      continue;
    print_task(p);
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
