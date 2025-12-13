#include "kstep.h"

static void pre_init(void) {
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_params.print_sched_softirq = true;
}

static struct task_struct *busy_task;

static void init(void) { busy_task = kstep_task_create(); }

static void body(void) {
  kstep_task_pin(busy_task, 1, 1);
  kstep_task_fork(busy_task, 20000);

  while (cpu_rq(1)->nr_running < 20000)
    kstep_sleep();

  kstep_tick_repeat(2000);
}

struct kstep_driver long_balance = {
    .name = "long_balance",
    .pre_init = pre_init,
    .init = init,
    .body = body,
};
