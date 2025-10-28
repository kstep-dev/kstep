#include "controller.h"
#include "logging.h"

static void controller_init(void) { TRACE_INFO("Noop controller initialized"); }

static void controller_body(void) {
  for (int i = 0; i < 10; i++) {
    TRACE_INFO("Noop controller step %d", i);
    if (i == 10) {
      return;
    }
  }
}

struct controller_ops controller_noop = {
    .name = "noop",
    .init = controller_init,
    .body = controller_body,
};
