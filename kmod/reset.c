#include "internal.h"

// Discard real-time execution history and start PELT at the mocked clock's epoch.
// Callers retain their distinct task load and task-group accounting initialization.
static void reset_sched_avg(struct sched_avg *avg) {
  memset(avg, 0, sizeof(*avg));
  avg->last_update_time = INIT_TIME_NS;
}

void kstep_reset_task(struct task_struct *p) {
  // reset generic task stats
  p->nivcsw = 0;
  p->nvcsw = 0;

  // reset sched entity stats
  p->se.exec_start = 0;
  p->se.sum_exec_runtime = 0;
  p->se.prev_sum_exec_runtime = 0;
  p->se.nr_migrations = 0;
  p->se.vruntime = INIT_TIME_NS;

// https://github.com/torvalds/linux/commit/86bfbb7ce4f67a88df2639198169b685668e7349
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  p->se.vlag = 0;
#endif

  // reset sched avg stats
  reset_sched_avg(&p->se.avg);
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

// Rebase an rq's clocks onto the mocked clock's epoch. The rq was last stamped with the real
// clock, which is ahead of the epoch, and update_rq_clock() drops a backwards step instead of
// resyncing -- so without this the rq clock, and with it vruntime, stays frozen all session. On
// x86 clock_task == clock without irq/steal accounting, and clock_pelt == clock_task at capacity
// 1024. Only the rq clocks: ___update_load_sum() resyncs last_update_time on its own.
static void reset_rq_clocks(struct rq *rq) {
  rq->clock = INIT_TIME_NS;
  rq->clock_task = INIT_TIME_NS;
// https://github.com/torvalds/linux/commit/23127296889fe84b0762b191b5d041e8ba6f2599
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 1, 0)
  rq->clock_pelt = INIT_TIME_NS;
  rq->lost_idle_time = 0;
#endif
// v5.19 "sched/fair: Decay task PELT values during wakeup migration"
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
  rq->clock_pelt_idle = INIT_TIME_NS;
  rq->clock_idle = INIT_TIME_NS;
#endif
}

static void kstep_reset_runqueue(struct rq *rq) {
  KSYM_IMPORT(sysctl_sched_migration_cost);
  reset_rq_clocks(rq);
  rq->avg_idle = 2 * *KSYM_sysctl_sched_migration_cost;
  rq->max_idle_balance_cost = *KSYM_sysctl_sched_migration_cost;
  rq->idle_stamp = INIT_TIME_NS;
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
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
  rq->cfs.sum_w_vruntime = 0;
  rq->cfs.sum_weight = 0;
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
  rq->cfs.avg_vruntime = 0;
  rq->cfs.avg_load = 0;
#endif
  reset_sched_avg(&rq->cfs.avg);

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

// Task groups accumulated PELT load in real time before the clock was mocked (tasks run on CPU 0
// while created and moved into their cgroups). It feeds tg->load_avg and with it the group
// entities' weights on every CPU, so leaving it makes runs differ. Root cfs_rqs are reset above.
static void kstep_reset_task_groups(void) {
  KSYM_IMPORT(task_groups);
  KSYM_IMPORT(root_task_group);
  struct task_group *tg;

  list_for_each_entry_rcu(tg, KSYM_task_groups, list) {
    if (tg == KSYM_root_task_group)
      continue;
    atomic_long_set(&tg->load_avg, 0);
    for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
      struct cfs_rq *cfs_rq = tg->cfs_rq[cpu];
      struct sched_entity *se = tg->se[cpu];

      reset_sched_avg(&cfs_rq->avg);
      cfs_rq->tg_load_avg_contrib = 0;
      cfs_rq->propagate = 0;
      cfs_rq->prop_runnable_sum = 0;
      reset_sched_avg(&se->avg);
    }
  }
}

// CPU 0 reads the same mocked clock as the test CPUs, so its rq needs the same rebase or nothing
// on the control CPU is scheduled on an advancing clock again. Clocks only: its tasks keep their
// real-time vruntime and PELT history (kstep_reset_tasks skips them) and stay consistent among
// themselves. Under the rq lock, unlike the test CPUs': CPU 0 keeps its timer tick, so it is live.
static void kstep_reset_control_rq(void) {
  // raw_spin_rq_lock_irqsave() expands to these two, which the kernel does not export.
  KSYM_IMPORT(raw_spin_rq_lock_nested);
  KSYM_IMPORT(raw_spin_rq_unlock);
  struct rq *rq = cpu_rq(0);
  unsigned long flags;

  local_irq_save(flags);
  KSYM_raw_spin_rq_lock_nested(rq, 0);
  reset_rq_clocks(rq);
  rq->idle_stamp = 0; // 0 means "not idle"; a real-clock stamp would skew avg_idle on the next wakeup
  KSYM_raw_spin_rq_unlock(rq);
  local_irq_restore(flags);
}

void kstep_reset_runqueues(void) {
  for_each_test_cpu(cpu)
    kstep_reset_runqueue(cpu_rq(cpu));
  kstep_reset_control_rq();
  kstep_reset_task_groups();
  TRACE_INFO("Reset runqueues state");
}

void kstep_reset_cpumask(void) {
  KSYM_IMPORT_TYPED(int, distribute_cpu_mask_prev);
// https://github.com/torvalds/linux/commit/46a87b3851f0d6eb05e6d83d5c5a30df0eca8f76
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
  for_each_test_cpu(cpu) {
    int *ptr = per_cpu_ptr(KSYM_distribute_cpu_mask_prev, cpu);
    *ptr = 0;
  }
  TRACE_INFO("Reset cpumask");
#endif
}

// Disable the fair dl_server by giving it zero runtime.
void kstep_reset_dl_server(void) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
  KSYM_IMPORT(dl_server_apply_params);
  for_each_test_cpu(cpu) {
    u64 runtime = 0;
    u64 period = 1000 * NSEC_PER_MSEC;
    KSYM_dl_server_apply_params(&cpu_rq(cpu)->fair_server, runtime, period, 1);
  }
#endif
}
