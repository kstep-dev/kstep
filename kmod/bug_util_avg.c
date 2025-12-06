#include "kstep.h"

static void pre_init(void) {
  kstep_params.step_interval_us = 1000;
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = true;
}

static struct task_struct *tasks[2];

static void init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void body(void) {
  // set the first task to fifo
  kstep_task_fifo(tasks[0]);

  // fake the frequency of cpu 1 to 50% of the base frequency
  kstep_set_cpu_freq(1, SCHED_CAPACITY_SCALE >> 1);

  // tick until the util_avg becomes 100%
  kstep_tick_repeat(600);

  // pause the fifo task
  kstep_task_pause(tasks[0]);

  // wait for another 2 ticks (2ms)
  kstep_tick_repeat(2);

  // wake up and set antoher task to fifo
  kstep_task_fifo(tasks[1]);

  // tick for another 600 ticks (600ms) to show the impact
  kstep_tick_repeat(600);
}

struct kstep_driver util_avg = {
    .name = "util_avg",
    .pre_init = pre_init,
    .init = init,
    .body = body,
};
