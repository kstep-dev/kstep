#include <linux/delay.h>
#include <linux/kthread.h>

#include "kstep.h"

#define TARGET_TASK "test-proc"

static struct task_struct *busy_task;

static void controller_init(void) {
  busy_task = poll_task(TARGET_TASK);
  reset_task_stats(busy_task);
  kstep_sleep();
}

DECLARE_PER_CPU(unsigned long, arch_freq_scale);

static struct task_struct * find_ff_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0 || p == busy_task)
      continue;
    if (p->sched_class == ksym.rt_sched_class)
      return p;
  }
  return NULL;
}
static void controller_body(void) {
  // fork 1 process using fifo sched class
  send_sigcode(busy_task, SIGCODE_FORK_FF, 1);

  // fake the frequency of cpu 2 to 50% of the base frequency
  *per_cpu_ptr(ksym.arch_freq_scale, 2) = 512L;

  // tick until the util_avg becomes 100%
  for (int i = 0; i < 600; i++) {
    call_tick_once(true);
  }

  // pause the fifo task for
  struct task_struct *ff_task = find_ff_task();
  
  if (ff_task) {
    send_sigcode(ff_task, SIGCODE_PAUSE, 0);
  } else {
    TRACE_ERR("no FF task found");
  }

  // wait for another 2 ticks (2ms)
  for (int i = 0; i < 2; i++) {
    call_tick_once(true);
  }

  // start another fifo task
  send_sigcode(busy_task, SIGCODE_FORK_FF, 1);

  // tick for another 600 ticks (600ms) to show the impact
  for (int i = 0; i < 600; i++) {
    call_tick_once(true);
  }
}

struct controller_ops controller_17e3e88 = {
    .name = "17e3e88",
    .init = controller_init,
    .body = controller_body,
};
