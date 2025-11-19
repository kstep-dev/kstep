#include "kstep.h"

#define TARGET_TASK "test-proc"

static void controller_init(void) {}

static void controller_body(void) {
  static struct task_struct *busy_task;
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);

  kstep_sleep();

  for (int i = 0; i < 10; i++) {
    TRACE_INFO("Noop controller step %d", i);
    send_sigcode(busy_task, SIGCODE_FORK, 1);
    call_tick_once();
  }
}

struct controller_ops controller_noop = {
    .name = "noop",
    .init = controller_init,
    .body = controller_body,
};
