#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/reboot.h>
#include <linux/version.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"

void call_tick_once(bool print_tasks_flag) {
  if (print_tasks_flag) {
    print_tasks();
  }
  sched_clock_tick();

  // Call tick function
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
    smp_call_function_single(cpu, (void *)ksym.sched_tick, NULL, 0);
#else
    smp_call_function_single(cpu, (void *)ksym.scheduler_tick, NULL, 0);
#endif
    udelay(SIM_INTERVAL_US);
  }
}

static void disable_timer_ticks(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    ksym.tick_sched_timer_dying(cpu);
  }
}

static void enable_timer_ticks(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.tick_setup_sched_timer,
                             (void *)true, 0);
  }
}

static void disable_ticks(void) {
  // disable ticks on all cpus to avoid jiffies from being updated
  for (int cpu = 0; cpu < num_online_cpus(); cpu++) {
    ksym.tick_offline_cpu(cpu);
  }
}

static void disable_workqueue(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.workqueue_offline_cpu,
                             (void *)(intptr_t)cpu, 1);
  }
}

static void move_kthreads(void) {
  struct task_struct *p;
  for_each_process(p) {
    // Skip if 0 is the only allowed cpu
    if (cpumask_test_cpu(0, &p->cpus_mask) &&
        cpumask_weight(&p->cpus_mask) == 1) {
      continue;
    }
    // skip non-kthreads
    if (!(p->flags & PF_KTHREAD)) {
      continue;
    }

    if (is_sys_kthread(p)) {
      reset_task_stats(p);
      continue;
    }
    set_cpus_allowed_ptr(p, cpumask_of(0));
    wake_up_process(p);
    udelay(SIM_INTERVAL_US); // sometimes kworker/1:2H can be started very late
                             // and miss the move_kthreads
  }
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
    rq->cfs.avg_vruntime = 0;
    rq->cfs.avg_load = 0;
    memset(&rq->cfs.avg, 0, sizeof(struct sched_avg));
    rq->cfs.avg.last_update_time = INIT_TIME_NS;

    // reset sched domain
    for (struct sched_domain *sd = rcu_dereference_check_sched_domain(rq->sd);
         sd; sd = sd->parent) {
      sd->last_balance = jiffies;
      sd->balance_interval = sd->min_interval;
      sd->nr_balance_failed = 0;
    }
  }
}

void controller_run(struct controller_ops *ops) {
  disable_timer_ticks();
  disable_ticks();
  disable_workqueue();
  sched_clock_init();
  sched_clock_set(INIT_TIME_NS);
  move_kthreads();
  reset_rq();

  TRACE_INFO("Initializing controller %s", ops->name);
  ops->init();
  udelay(SIM_INTERVAL_US);
  TRACE_INFO("Running controller %s", ops->name);
  ops->body();
  TRACE_INFO("Exiting controller %s", ops->name);
  kernel_power_off();

  sched_clock_exit();
  enable_timer_ticks();
}
