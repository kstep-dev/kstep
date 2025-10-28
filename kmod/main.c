#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/reboot.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "utils.h"

static char controller_name[32] = "noop";
module_param_string(controller_name, controller_name, sizeof(controller_name),
                    0644);
MODULE_PARM_DESC(controller_name, "Controller name to run");

static void common_init(void) {
  // Disable timer ticks and workqueue on all controlled CPUs
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    ksym.tick_sched_timer_dying(cpu);
    smp_call_function_single(cpu, (void *)ksym.workqueue_offline_cpu,
                             (void *)(intptr_t)cpu, 1);
  }

  // Move non-essential kernel threads to CPU 0
  {
    struct task_struct *p;
    for_each_process(p) {
      if (task_cpu(p) == 0)
        continue;
      if (strncmp(p->comm, "cpuhp/", 6) == 0 ||
          strncmp(p->comm, "migration/", 10) == 0 ||
          strncmp(p->comm, "ksoftirqd/", 10) == 0) {
        p->se.vruntime = INIT_TIME_NS;
        p->se.deadline = INIT_TIME_NS;
        p->se.sum_exec_runtime = INIT_TIME_NS;
        continue;
      }
      set_cpus_allowed_ptr(p, cpumask_of(0));
      wake_up_process(p);
    }
  }

  sched_clock_init();
  sched_clock_set(INIT_TIME_NS);

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = cpu_rq(cpu);
    ksym.update_rq_clock(rq);

    rq->avg_idle = 2 * *ksym.sysctl_sched_migration_cost;
    rq->max_idle_balance_cost = *ksym.sysctl_sched_migration_cost;
    rq->nr_switches = 0;

    rq->cfs.min_vruntime = INIT_TIME_NS;

    for (struct sched_domain *sd = rcu_dereference_check_sched_domain(rq->sd);
         sd; sd = sd->parent) {
      sd->last_balance = jiffies;
      sd->balance_interval = sd->min_interval;
      sd->nr_balance_failed = 0;
    }
  }
}

static void common_step(int iter) {
  // Call tick function on one cpu at at time, excluding CPU 0
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.sched_tick, NULL, 1);
    msleep(SIM_INTERVAL_MS);
  }

  // Update clock
  sched_clock_inc(TICK_INTERVAL_NS);
}

static void common_exit(void) {
  sched_clock_exit();
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.tick_setup_sched_timer,
                             (void *)true, 0);
  }
}

static int controller(void *data) {
  struct controller_ops *ops = data;
  common_init();
  ops->init();
  int iter = 0;
  while (!kthread_should_stop()) {
    if (ops->step(iter) != 0) {
      break;
    }
    common_step(iter);
    iter++;
  }
  ops->exit();
  common_exit();
  kernel_power_off();
  return 0;
}

static struct task_struct *controller_task;

static int __init kmod_init(void) {
  ksym_init();
  sched_trace_init();
  cpu_controlled_mask_init();
  struct controller_ops *ops = get_controller_ops(controller_name);
  if (ops == NULL) {
    TRACE_ERR("Controller %s not found", controller_name);
    return -EINVAL;
  }
  controller_task = kthread_create(controller, ops, ops->name);
  set_cpus_allowed_ptr(controller_task, cpumask_of(0));
  wake_up_process(controller_task);
  return 0;
}

static void __exit kmod_exit(void) {
  kthread_stop(controller_task);
  sched_trace_exit();
}

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");
