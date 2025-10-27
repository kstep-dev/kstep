#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched/clock.h>

#include "logging.h"

// Linux private headers
#include <kernel/sched/sched.h>

#define KSYM_VAR_LIST X(struct rq, runqueues)
#include "ksym.h"

// Module parameters
static char *func_names[32];
static int func_count = 0;
module_param_array(func_names, charp, &func_count, 0644);
MODULE_PARM_DESC(func_names, "Function names to trace");

static void sched_tick_cb(unsigned long ip, unsigned long parent_ip,
                          struct ftrace_ops *op, struct ftrace_regs *fregs) {
  int cpu = smp_processor_id();
  if (cpu == 0)
    return;
  struct rq *rq = per_cpu_ptr(ksym_runqueues, cpu);
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
          cpu, rq->nr_running, rq->nr_switches, rq->nr_uninterruptible,
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
          cpu, rq->cfs.min_vruntime, rq->cfs.avg_vruntime, rq->cfs.nr_queued,
          rq->cfs.h_nr_runnable, rq->cfs.h_nr_queued, rq->cfs.h_nr_idle,
          rq->cfs.load.weight, rq->cfs.avg.load_avg, rq->cfs.avg.runnable_avg,
          rq->cfs.avg.util_avg, rq->cfs.avg.util_est, rq->cfs.removed.load_avg,
          rq->cfs.removed.util_avg, rq->cfs.removed.runnable_avg,
          rq->cfs.tg_load_avg_contrib, atomic_long_read(&rq->cfs.tg->load_avg));
  pr_info("{\"rt_rq[%d]\": {\"rt_nr_running\": %u}}", cpu,
          rq->rt.rt_nr_running);
  pr_info("{\"dl_rq[%d]\": {\"dl_nr_running\": %u, \"bw\": %llu, "
          "\"total_bw\": %llu}}",
          cpu, rq->dl.dl_nr_running, rq->rd->dl_bw.bw, rq->rd->dl_bw.total_bw);
  struct task_struct *g;
  struct task_struct *p;
  for_each_process_thread(g, p) {
    if (task_cpu(p) != cpu)
      continue;
    pr_info("{"
            "\"tasks[%d]\": "
            "{"
            "\"cpu\": %d, "
            "\"on_cpu\": %d, "
            "\"task_state\": \"%c\", "
            "\"comm\": \"%s\", "
            "\"pid\": %d, "
            "\"ppid\": %d, "
            "\"vruntime\": %llu, "
            "\"deadline\": %llu, "
            "\"slice\": %llu, "
            "\"sum-exec\": %llu, "
            "\"switches\": %lu, "
            "\"prio\": %d, "
            "\"wait-time\": %llu, "
            "\"sum-sleep\": %llu, "
            "\"sum-block\": %llu, "
            "\"node\": %d"
            "}"
            "}",
            task_pid_nr(p), cpu, p->on_cpu, task_state_to_char(p), p->comm,
            task_pid_nr(p), task_ppid_nr(p), p->se.vruntime, p->se.deadline,
            p->se.slice, p->se.sum_exec_runtime, p->nvcsw + p->nivcsw, p->prio,
            p->stats.wait_sum, p->stats.sum_sleep_runtime,
            p->stats.sum_block_runtime, task_node(p));
  }
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
    {.callback = &update_rq_clock_cb, .name = "update_rq_clock"},
    {.callback = &sched_balance_domains_cb, .name = "sched_balance_domains"},
    {.callback = &sched_balance_rq_cb, .name = "sched_balance_rq"},
};

static void trace_func_enable(struct trace_func_info *info) {
  info->op.func = info->callback;
  info->op.flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED;
  ftrace_set_filter(&info->op, info->name, strlen(info->name), 1);
  if (register_ftrace_function(&info->op)) {
    TRACE_ERR("Failed to register %s", info->name);
  } else {
    TRACE_INFO("Registered %s", info->name);
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

static int __init kmod_init(void) {
  init_kernel_symbols();
  for (int i = 0; i < func_count; i++) {
    char *name = func_names[i];
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

static void __exit kmod_exit(void) {
  for (int i = 0; i < ARRAY_SIZE(trace_func_infos); i++) {
    struct trace_func_info *info = &trace_func_infos[i];
    trace_func_disable(info);
  }
  TRACE_INFO("Scheduler trace uninitialized");
}

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler trace");
