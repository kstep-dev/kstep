#include "kstep.h"

static void pre_init(void) {
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_trace_rebalance();
}

static struct task_struct *busy_task;

static void init(void) { busy_task = kstep_task_create(); }

static void body(void) {
  for (int i = 0; i < 1000; i++)
    kstep_task_fork_pin(busy_task, 100, 1, 1);
  kstep_tick_repeat(1000);
}

struct kstep_driver long_balance = {
    .name = "long_balance",
    .pre_init = pre_init,
    .init = init,
    .body = body,
};
