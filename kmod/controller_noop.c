#include <linux/reboot.h>

#include "controller.h"
#include "logging.h"

static int noop_init(void) {
  TRACE_INFO("Noop controller initialized");
  return 0;
}

static int noop_step(int iter) {
  TRACE_INFO("Noop controller step %d", iter);
  if (iter == 10) {
    return 1;
  }
  return 0;
}

static int noop_exit(void) {
  TRACE_INFO("Noop controller exited");
  kernel_power_off();
  return 0;
}

struct controller_ops controller_noop = {
    .name = "noop",
    .init = noop_init,
    .step = noop_step,
    .exit = noop_exit,
};
