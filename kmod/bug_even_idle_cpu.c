#include "kstep.h"

static void controller_pre_init(void) {
  kstep_params.step_interval_us = 1000;
  kstep_params.print_nr_running = true;
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
}

static void controller_body(void) {
  // making the nr_running on cpu 4-7 to [1, 0, 3, 1]
  send_sigcode(busy_task, SIGCODE_PIN, 4);
  send_sigcode3(busy_task, SIGCODE_FORK_PIN_RANGE, 3, 6, 6);
  send_sigcode3(busy_task, SIGCODE_FORK_PIN_RANGE, 1, 7, 7);

  kstep_tick_repeat(200);

  struct task_struct *pin_task;
  for_each_process(pin_task) {
    if (strcmp(pin_task->comm, busy_task->comm) != 0 || pin_task == busy_task)
      continue;
    if (task_cpu(pin_task) == 6)
      send_sigcode2(pin_task, SIGCODE_PIN, 4, 6);
  }

  kstep_tick_repeat(1000);
}

struct controller_ops controller_even_idle_cpu = {
    .name = "even_idle_cpu",
    .pre_init = controller_pre_init,
    .body = controller_body,
};
