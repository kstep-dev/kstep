#include "internal.h"

static void kstep_reset_task(struct task_struct *p) {
  // reset generic task stats
  p->nivcsw = 0;
  p->nvcsw = 0;

  // reset sched entity stats
  p->se.exec_start = 0;
  p->se.sum_exec_runtime = 0;
  p->se.prev_sum_exec_runtime = 0;
  p->se.nr_migrations = 0;
  p->se.vruntime = INIT_TIME_NS;
  // p->se.deadline = INIT_TIME_NS;

// https://github.com/torvalds/linux/commit/86bfbb7ce4f67a88df2639198169b685668e7349
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  p->se.vlag = 0;
#endif

  // reset sched avg stats
  memset(&p->se.avg, 0, sizeof(struct sched_avg));
  p->se.avg.last_update_time = INIT_TIME_NS;
  p->se.avg.load_avg = scale_load_down(p->se.load.weight);
}

void kstep_reset_tasks(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) != 0)
      kstep_reset_task(p);
  }
  TRACE_INFO("Reset tasks state");
}

static void kstep_reset_runqueue(struct rq *rq) {
  // reset rq
  KSYM_IMPORT(update_rq_clock);
  KSYM_IMPORT(sysctl_sched_migration_cost);
  KSYM_update_rq_clock(rq);
  rq->avg_idle = 2 * *KSYM_sysctl_sched_migration_cost;
  rq->max_idle_balance_cost = *KSYM_sysctl_sched_migration_cost;
  rq->nr_switches = 0;
  rq->next_balance = INITIAL_JIFFIES + nsecs_to_jiffies(INIT_TIME_NS);

  // reset cfs rq
// https://github.com/torvalds/linux/commit/79f3f9bedd149ea438aaeb0fb6a083637affe205
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  rq->cfs.zero_vruntime = INIT_TIME_NS;
#else
  rq->cfs.min_vruntime = INIT_TIME_NS;
#endif

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
// https://github.com/torvalds/linux/commit/dcbc9d3f0e594223275a18f7016001889ad35eff (avg_vruntime -> sum_w_vruntime)
// https://github.com/torvalds/linux/commit/4ff674fa986c27ec8a0542479258c92d361a2566 (avg_load -> sum_weight)
// https://github.com/torvalds/linux/commit/dcbc9d3f0e594223275a18f7016001889ad35eff (avg_vruntime -> sum_w_vruntime)
// https://github.com/torvalds/linux/commit/4ff674fa986c27ec8a0542479258c92d361a2566 (avg_load -> sum_weight)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  rq->cfs.sum_w_vruntime = 0;
  rq->cfs.sum_weight = 0;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  rq->cfs.avg_vruntime = 0;
  rq->cfs.avg_load = 0;
#endif
  memset(&rq->cfs.avg, 0, sizeof(struct sched_avg));
  rq->cfs.avg.last_update_time = INIT_TIME_NS;

  // reset sched domain
  struct sched_domain *sd;
  for_each_domain(rq->cpu, sd) {
    sd->last_balance = jiffies;
    sd->balance_interval = sd->min_interval;
    sd->nr_balance_failed = 0;
    sd->max_newidle_lb_cost = 0;
// https://github.com/torvalds/linux/commit/e60b56e46b384cee1ad34e6adc164d883049c6c3
// Note: dev kernels v5.15-rc+ already have the rename before 5.16 release
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
    sd->last_decay_max_lb_cost = jiffies;
#else
    sd->next_decay_max_lb_cost = jiffies;
#endif
  }
}

void kstep_reset_runqueues(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++)
    kstep_reset_runqueue(cpu_rq(cpu));
  TRACE_INFO("Reset runqueues state");
}

void kstep_reset_cpumask(void) {
  KSYM_IMPORT_TYPED(int, distribute_cpu_mask_prev);
// https://github.com/torvalds/linux/commit/46a87b3851f0d6eb05e6d83d5c5a30df0eca8f76
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    int *ptr = per_cpu_ptr(KSYM_distribute_cpu_mask_prev, cpu);
    *ptr = 0;
  }
  TRACE_INFO("Reset cpumask");
#endif
}
