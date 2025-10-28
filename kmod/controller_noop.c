#include "controller.h"
#include "logging.h"

static int controller_init(void) {
  TRACE_INFO("Noop controller initialized");
  return 0;
}

static int controller_step(int iter) {
  TRACE_INFO("Noop controller step %d", iter);
  if (iter == 10) {
    return 1;
  }
  return 0;
}

static int controller_exit(void) {
  TRACE_INFO("Noop controller exited");
  return 0;
}

struct controller_ops controller_noop = {
    .name = "noop",
    .init = controller_init,
    .step = controller_step,
    .exit = controller_exit,
};
