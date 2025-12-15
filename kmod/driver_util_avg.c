#include "kstep.h"

static struct task_struct *tasks[2];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // set the first task to fifo
  kstep_task_fifo(tasks[0]);

  // fake the frequency of cpu 1 to 50% of the base frequency
  kstep_cpu_set_freq(1, SCHED_CAPACITY_SCALE >> 1);

  // tick until the util_avg becomes 100%
  kstep_tick_repeat(600);

  // pause the fifo task
  kstep_task_pause(tasks[0]);

  // wait for another 2 ticks (2ms)
  kstep_tick_repeat(2);

  // wake up and set another task to fifo
  kstep_task_fifo(tasks[1]);

  // tick for another 600 ticks (600ms) to show the impact
  kstep_tick_repeat(600);
}

struct kstep_driver util_avg = {
    .name = "util_avg",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
};
