#include "kstep.h"

static void pre_init(void) {
  kstep_params.print_lb_events = true;
  kstep_params.print_nr_running = true;
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_params.step_interval_us = 1000;
}

static void body(void) {
  // making the nr_running on cpu 4-7 to [1, 0, 3, 1]
  kstep_task_pin(busy_task, 4, 4);
  kstep_task_fork_pin(busy_task, 3, 6, 6);
  kstep_task_fork_pin(busy_task, 1, 7, 7);

  kstep_tick_repeat(1000);

  struct task_struct *pin_task;
  for_each_process(pin_task) {
    if (strcmp(pin_task->comm, busy_task->comm) != 0 || pin_task == busy_task)
      continue;
    if (task_cpu(pin_task) == 6)
      kstep_task_pin(pin_task, 4, 6);
  }

  kstep_tick_repeat(301);
}

struct kstep_driver extra_balance = {
    .name = "extra_balance",
    .pre_init = pre_init,
    .body = body,
};
