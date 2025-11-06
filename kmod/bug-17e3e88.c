#include <linux/delay.h>
#include <linux/kthread.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;

static void controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  udelay(SIM_INTERVAL_US);
}

DECLARE_PER_CPU(unsigned long, arch_freq_scale);

static void controller_body(void) {
  // fork 5 processes
  send_sigcode(busy_task, SIGCODE_FORK, 5);

  *per_cpu_ptr(ksym.arch_freq_scale, 1) = 512L;

  for (int i = 0; i < 20; i++) {
    call_tick_once(true);
  }
}

struct controller_ops controller_17e3e88 = {
    .name = "17e3e88",
    .init = controller_init,
    .body = controller_body,
};
