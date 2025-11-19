#include "kstep.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;

static void controller_pre_init(void) {
  kstep_params.print_tasks = false;
  kstep_trace_rebalance();
}

static void controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  kstep_sleep();
}

static void controller_body(void) {
  for (int i = 0; i < 1000; i++) {
    send_sigcode2(busy_task, SIGCODE_FORK_PIN, 100, 1);
  }

  for (int i = 0; i < 1000; i++) {
    call_tick_once();
  }
}

struct controller_ops controller_2feab24 = {
    .name = "2feab24",
    .pre_init = controller_pre_init,
    .init = controller_init,
    .body = controller_body,
};
