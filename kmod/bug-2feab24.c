#include <linux/delay.h>
#include <linux/kthread.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;

static void controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  udelay(SIM_INTERVAL_US);
}

static void controller_body(void) {
  for (int i = 0; i < 1000; i++) {
    send_sigcode2(busy_task, SIGCODE_FORK_PIN, 100, 1);
  }

  for (int i = 0; i < 1000; i++) {
    call_tick_once(false);
  }
}

struct controller_ops controller_2feab24 = {
    .name = "2feab24",
    .init = controller_init,
    .body = controller_body,
};
