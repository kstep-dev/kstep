#define TRACE_LEVEL LOGLEVEL_DEBUG

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/reboot.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "utils.h"

static char *sys_kthread_comms[] = {
    "cpuhp/",
    "migration/",
    "ksoftirqd/",
};

static int is_sys_kthread(struct task_struct *p) {
  for (int i = 0; i < ARRAY_SIZE(sys_kthread_comms); i++) {
    if (strstarts(p->comm, sys_kthread_comms[i]))
      return 1;
  }
  return 0;
}

void print_tasks(void) {
  struct task_struct *p;

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    TRACE_INFO("- CPU %d running=%d, switches=%3lld, clock=%lld, avg_load=%lld",
               cpu,
               rq->nr_running - (rq->cfs.h_nr_queued - rq->cfs.h_nr_runnable),
               rq->nr_switches, rq->clock, rq->cfs.avg_load);
  }

  int min_pid = INT_MAX;
  for_each_process(p) {
    if (task_cpu(p) == 0 || is_sys_kthread(p))
      continue;
    if (task_pid_nr(p) < min_pid)
      min_pid = task_pid_nr(p);
  }

  TRACE_DEBUG("\t%3s %c%s %5s %5s %12s %12s %9s", "CPU", ' ', "S", "PID",
              "PPID", "vruntime", "sum-exec", "switches");
  TRACE_DEBUG(
      "\t-------------------------------------------------------------");

  for_each_process(p) {
    if (task_cpu(p) == 0 || is_sys_kthread(p))
      continue;
    TRACE_DEBUG("\t%3d %c%c %5d %5d %12lld %12lld %4lu+%-4lu", task_cpu(p),
                p->on_cpu ? '>' : ' ', task_state_to_char(p),
                task_pid_nr(p) - min_pid, task_ppid_nr(p), p->se.vruntime,
                p->se.sum_exec_runtime, p->nvcsw, p->nivcsw);
  }
}

void call_tick_once(void) {
  print_tasks();
  sched_clock_inc(TICK_INTERVAL_NS);

  // Call tick function
  int cpu;
  for_each_controlled_cpu(cpu) {
    smp_call_function_single(cpu, (void *)ksym.sched_tick, NULL, 0);
    msleep(SIM_INTERVAL_MS);
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

static void disable_workqueue(void) {
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.workqueue_offline_cpu,
                             (void *)(intptr_t)cpu, 1);
  }
}

static void move_kthreads(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (task_cpu(p) == 0)
      continue;
    if (is_sys_kthread(p)) {
      reset_task_stats(p);
      continue;
    }
    set_cpus_allowed_ptr(p, cpumask_of(0));
    wake_up_process(p);
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
  disable_workqueue();
  sched_clock_init();
  sched_clock_set(INIT_TIME_NS);
  move_kthreads();
  reset_rq();

  TRACE_INFO("Initializing controller %s", ops->name);
  ops->init();
  msleep(SIM_INTERVAL_MS);
  TRACE_INFO("Running controller %s", ops->name);
  ops->body();
  TRACE_INFO("Exiting controller %s", ops->name);
  kernel_power_off();

  sched_clock_exit();
  enable_timer_ticks();
}
