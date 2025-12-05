#include "kstep.h"

static void pre_init(void) {
  kstep_params.step_interval_us = 10;
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = true;
}

static struct task_struct *find_ff_task(void) {
  struct task_struct *p;
  for_each_process(p) {
    if (strcmp(p->comm, busy_task->comm) != 0 || p == busy_task)
      continue;
    if (p->sched_class == ksym.rt_sched_class)
      return p;
  }
  return NULL;
}
static void body(void) {
  // fork 1 process using fifo sched class
  kstep_task_signal(busy_task, SIGCODE_FORK_FF, 1, 0, 0);

  // fake the frequency of cpu 2 to 50% of the base frequency
  kstep_set_cpu_freq(2, SCHED_CAPACITY_SCALE >> 1);

  // tick until the util_avg becomes 100%
  kstep_tick_repeat(600);

  // pause the fifo task for
  struct task_struct *ff_task = find_ff_task();

  if (ff_task) {
    kstep_task_pause(ff_task);
  } else {
    TRACE_ERR("no FF task found");
  }

  // wait for another 2 ticks (2ms)
  kstep_tick_repeat(2);

  // start another fifo task
  kstep_task_signal(busy_task, SIGCODE_FORK_FF, 1, 0, 0);

  // tick for another 600 ticks (600ms) to show the impact
  kstep_tick_repeat(600);
}

struct kstep_driver util_avg = {
    .name = "util_avg",
    .pre_init = pre_init,
    .body = body,
};
