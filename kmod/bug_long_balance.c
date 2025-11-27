#include "kstep.h"

static void controller_pre_init(void) {
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_trace_rebalance();
}

static void controller_init(void) {}

static void controller_body(void) {
  for (int i = 0; i < 1000; i++) {
    send_sigcode2(busy_task, SIGCODE_FORK_PIN, 100, 1);
  }

  for (int i = 0; i < 1000; i++) {
    kstep_tick();
  }
}

struct controller_ops controller_long_balance = {
    .name = "long_balance",
    .pre_init = controller_pre_init,
    .init = controller_init,
    .body = controller_body,
};
