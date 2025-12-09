#include "kstep.h"

static void pre_init(void) {
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_params.print_rebalance_overhead = true;
}

static struct task_struct *busy_task;

static void init(void) { busy_task = kstep_task_create(); }

static void body(void) {
  kstep_task_fork_pin(busy_task, 10000, 1, 1);

  while (cpu_rq(1)->nr_running < 10000)
    kstep_sleep();

  kstep_tick_repeat(1000);
}

struct kstep_driver long_balance = {
    .name = "long_balance",
    .pre_init = pre_init,
    .init = init,
    .body = body,
};
