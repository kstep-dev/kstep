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

#if 0
void print_rq_json(struct rq *rq) {
  int h_nr_runnable_val = 0, h_nr_queued_val = 0, h_nr_idle_val = 0,
      nr_queued_val = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
  h_nr_runnable_val = rq->cfs.h_nr_runnable;
  h_nr_queued_val = rq->cfs.h_nr_queued;
  h_nr_idle_val = rq->cfs.h_nr_idle;
  nr_queued_val = rq->cfs.nr_queued;
#endif

// https://github.com/torvalds/linux/commit/11137d384996bb05cf33c8163db271e1bac3f4bf
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 8, 0)
  unsigned int util_est = rq->cfs.avg.util_est;
#else
  unsigned int util_est = rq->cfs.avg.util_est.enqueued;
#endif

  pr_info("{"
          "\"rq[%d]\": "
          "{"
          "\"nr_running\": %d, "
          "\"nr_switches\": %llu, "
          "\"nr_uninterruptible\": %d, "
          "\"next_balance\": %lu, "
          "\"curr->pid\": %d, "
          "\"clock\": %llu, "
          "\"clock_task\": %llu, "
          "\"avg_idle\": %llu, "
          "\"max_idle_balance_cost\": %llu"
          "}"
          "}",
          rq->cpu, rq->nr_running, rq->nr_switches, rq->nr_uninterruptible,
          rq->next_balance, rq->curr->pid, rq->clock, rq->clock_task,
          rq->avg_idle, rq->max_idle_balance_cost);
  pr_info("{"
          "\"cfs_rq[%d]\": "
          "{"
          "\"min_vruntime\": %llu, "
          "\"avg_vruntime\": %llu, "
          "\"nr_queued\": %u, "
          "\"h_nr_runnable\": %u, "
          "\"h_nr_queued\": %u, "
          "\"h_nr_idle\": %u, "
          "\"load\": %lu, "
          "\"load_avg\": %lu, "
          "\"runnable_avg\": %lu, "
          "\"util_avg\": %lu, "
          "\"util_est\": %d, "
          "\"removed.load_avg\": %lu, "
          "\"removed.util_avg\": %lu, "
          "\"removed.runnable_avg\": %lu, "
          "\"tg_load_avg_contrib\": %lu, "
          "\"tg_load_avg\": %ld"
          "}"
          "}",
          rq->cpu, rq->cfs.min_vruntime, rq->cfs.avg_vruntime, nr_queued_val,
          h_nr_runnable_val, h_nr_queued_val, h_nr_idle_val,
          rq->cfs.load.weight, rq->cfs.avg.load_avg, rq->cfs.avg.runnable_avg,
          rq->cfs.avg.util_avg, util_est, rq->cfs.removed.load_avg,
          rq->cfs.removed.util_avg, rq->cfs.removed.runnable_avg,
          rq->cfs.tg_load_avg_contrib, atomic_long_read(&rq->cfs.tg->load_avg));
  pr_info("{\"rt_rq[%d]\": {\"rt_nr_running\": %u}}", rq->cpu,
          rq->rt.rt_nr_running);
  pr_info("{\"dl_rq[%d]\": {\"dl_nr_running\": %u, \"bw\": %llu, "
          "\"total_bw\": %llu}}",
          rq->cpu, rq->dl.dl_nr_running, rq->rd->dl_bw.bw,
          rq->rd->dl_bw.total_bw);
}

void print_task_json(struct task_struct *p) {
  pr_info("{"
          "\"tasks[%d]\": "
          "{"
          "\"comm\": \"%s\", "
          "\"pid\": %d, "
          "\"ppid\": %d, "
          "\"cpu\": %d, "
          "\"on_cpu\": %d, "
          "\"task_state\": \"%c\", "
          "\"vruntime\": %llu, "
          "\"deadline\": %llu, "
          "\"slice\": %llu, "
          "\"sum-exec\": %llu, "
          "\"switches\": %lu, "
          "\"prio\": %d, "
          "\"node\": %d"
          "}"
          "}",
          task_pid_nr(p), p->comm, task_pid_nr(p), task_ppid_nr(p), task_cpu(p),
          p->on_cpu, task_state_to_char(p), p->se.vruntime, p->se.deadline,
          p->se.slice, p->se.sum_exec_runtime, p->nvcsw + p->nivcsw, p->prio,
          task_node(p));
}

void print_sd_json(struct sched_domain *sd) {
  pr_info("{"
          "\"sd[\"%s\"]\": "
          "{"
          "\"last_balance\": %lu, "
          "\"balance_interval\": %u, "
          "\"nr_balance_failed\": %u, "
          "\"min_interval\": %lu, "
          "\"max_interval\": %lu, "
          "\"max_newidle_lb_cost\": %llu, "
          "\"busy_factor\": %u, "
          "\"imbalance_pct\": %u, "
          "\"cache_nice_tries\": %u, "
          "\"imb_numa_nr\": %u, "
          "\"nohz_idle\": %d, "
          "\"flags\": %u, "
          "\"level\": %u, "
          "\"span_weight\": %u "
          "}"
          "}",
          sd->name, sd->last_balance, sd->balance_interval,
          sd->nr_balance_failed, sd->min_interval, sd->max_interval,
          sd->max_newidle_lb_cost, sd->busy_factor, sd->imbalance_pct,
          sd->cache_nice_tries, sd->imb_numa_nr, sd->nohz_idle, sd->flags,
          sd->level, sd->span_weight);
}

void print_sched_state_json(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    print_rq_json(cpu_rq(cpu));
  }
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct sched_domain *sd;
    for_each_domain(cpu, sd) { print_sd_json(sd); }
  }
  struct task_struct *g;
  struct task_struct *p;
  for_each_process_thread(g, p) {
    if (task_cpu(p) == 0)
      continue;
    print_task_json(p);
  }
}
#endif

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

int sched_trace_init(void) {
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

void sched_trace_exit(void) {
  for (int i = 0; i < ARRAY_SIZE(trace_func_infos); i++) {
    struct trace_func_info *info = &trace_func_infos[i];
    trace_func_disable(info);
  }
  TRACE_INFO("Scheduler trace uninitialized");
}
