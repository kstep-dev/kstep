#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/workqueue.h>

// Linux private headers
#include <kernel/sched/sched.h>

#include "internal.h"
#include "ksym.h"
#include "logging.h"
#include "sigcode.h"

#define SIM_INTERVAL_MS 10
#define TICK_INTERVAL_NS (1000ULL * 1000ULL)               // 1 ms
#define INIT_TIME_NS (10ULL * 1000ULL * 1000ULL * 1000ULL) // 10s
#define TARGET_TASK "test-proc"

static void send_sigcode(struct task_struct *p, enum sigcode code, int val) {
  struct kernel_siginfo info = {
      .si_signo = SIGUSR1,
      .si_code = code,
      .si_int = val,
  };
  send_sig_info(SIGUSR1, &info, p);
  TRACE_INFO("Sent %s (si_int=%d) to pid %d", sigcode_to_str[code], val,
             p->pid);
}

static struct task_struct *poll_target_task(void) {
  struct task_struct *p;
  while (1) {
    for_each_process(p) {
      if (strcmp(p->comm, TARGET_TASK) == 0)
        return p;
    }
    msleep(SIM_INTERVAL_MS);
    TRACE_INFO("Waiting for process %s to be created", TARGET_TASK);
  }
}

static void controller_init(void) {
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

  struct task_struct *p = poll_target_task();
  send_sigcode(p, SIGCODE_FORK, 4);
  p->se.vruntime = INIT_TIME_NS;
  p->nivcsw = 0;
  p->nvcsw = 0;

  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = per_cpu_ptr(ksym.runqueues, cpu);
    rq->avg_idle = 2 * *ksym.sysctl_sched_migration_cost;
    rq->max_idle_balance_cost = *ksym.sysctl_sched_migration_cost;
    rq->nr_switches = 0;

    rq->cfs.min_vruntime = INIT_TIME_NS;
  }
}

static void controller_exit(void) {
  sched_clock_exit();
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.tick_setup_sched_timer,
                             (void *)true, 0);
  }
}

static void controller_step(int iter) {
  // Update clock
  sched_clock_inc(TICK_INTERVAL_NS);
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    struct rq *rq = per_cpu_ptr(ksym.runqueues, cpu);
    // force balance at every tick
    rq->next_balance = 0;
    for (struct sched_domain *sd = rq->sd; sd; sd = sd->parent) {
      sd->last_balance = 0;
    }
  }

  // Call tick function on one cpu at at time, excluding CPU 0
  for (int cpu = 1; cpu < num_online_cpus(); cpu++) {
    smp_call_function_single(cpu, (void *)ksym.sched_tick, NULL, 1);
    msleep(SIM_INTERVAL_MS);
  }
}

static int controller(void *data) {
  controller_init();
  int iter = 0;
  while (!kthread_should_stop()) {
    controller_step(iter++);
  }
  controller_exit();
  return 0;
}

static struct task_struct *controller_task;

static int __init kmod_init(void) {
  ksym_init();
  sched_trace_init();
  controller_task = kthread_create(controller, NULL, "controller");
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
