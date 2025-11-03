#include "controller.h"
#include "logging.h"
#include "utils.h"

#define TARGET_TASK "test-proc"

static void controller_init(void) {}

static void controller_body(void) {
  static struct task_struct *busy_task;
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);

  for (int i = 0; i < 10; i++) {
    TRACE_INFO("Noop controller step %d", i);
    send_sigcode(busy_task, SIGCODE_FORK, 1);
    controller_tick();
  }
}

struct controller_ops controller_noop = {
    .name = "noop",
    .init = controller_init,
    .body = controller_body,
};
