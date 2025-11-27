#include "kstep.h"

#define TARGET_TASK "test-proc"
#define CGROUP_TASK "cgroup-proc"

static void controller_pre_init(void) {
  kstep_params.print_lb_events = true;
  kstep_params.print_nr_running = true;
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_params.step_interval_us = 1000;
}

static void controller_init(void) { send_sigcode(busy_task, SIGCODE_PIN, 1); }

static void controller_body(void) {
  // making the nr_running on cpu 4-7 to [1, 0, 3, 1]
  send_sigcode(busy_task, SIGCODE_PIN, 4);
  send_sigcode3(busy_task, SIGCODE_FORK_PIN_RANGE, 3, 6, 6);
  send_sigcode3(busy_task, SIGCODE_FORK_PIN_RANGE, 1, 7, 7);
  
  for (int i = 0; i < 1000; i++) {
    kstep_tick();
  }

  struct task_struct *pin_task;
  for_each_process(pin_task) {
    if (strcmp(pin_task->comm, TARGET_TASK) != 0 || pin_task == busy_task)
      continue;
    if (task_cpu(pin_task) == 6)
      send_sigcode2(pin_task, SIGCODE_PIN, 4, 6);
  }


  for(int i = 0; i < 1000; i++) {
    kstep_tick();
  }

}

struct controller_ops controller_extra_balance = {
    .name = "extra_balance",
    .pre_init = controller_pre_init,
    .init = controller_init,
    .body = controller_body,
};
