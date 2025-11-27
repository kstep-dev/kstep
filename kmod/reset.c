#include "kstep.h"

void reset_task_stats(struct task_struct *p) {
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

static void reset_rq(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);

    // reset rq
    ksym.update_rq_clock(rq);
    rq->avg_idle = 2 * *ksym.sysctl_sched_migration_cost;
    rq->max_idle_balance_cost = *ksym.sysctl_sched_migration_cost;
    rq->nr_switches = 0;
    rq->next_balance = INITIAL_JIFFIES + nsecs_to_jiffies(INIT_TIME_NS);

    // reset cfs rq
    rq->cfs.min_vruntime = INIT_TIME_NS;

// https://github.com/torvalds/linux/commit/af4cf40470c22efa3987200fd19478199e08e103
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 16, 0)
      sd->last_decay_max_lb_cost = jiffies;
#else
      sd->next_decay_max_lb_cost = jiffies;
#endif
    }
  }
}

static void reset_distribute_cpu_mask_prev(void) {
// https://github.com/torvalds/linux/commit/46a87b3851f0d6eb05e6d83d5c5a30df0eca8f76
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    int *ptr = per_cpu_ptr(ksym.distribute_cpu_mask_prev, cpu);
    *ptr = 0;
  }
#endif
}

void kstep_reset_sched_state(void) {
  reset_rq();
  reset_distribute_cpu_mask_prev();
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) != 0) {
      reset_task_stats(p);
    }
  }
}
