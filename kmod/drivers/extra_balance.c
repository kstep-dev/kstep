// https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3

#include "driver.h"
#include "internal.h" // cpu_rq

static struct task_struct *tasks[5];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // making the nr_running on cpu 4-7 to [1, 0, 3, 1]
  int cpus[] = {4, 6, 6, 6, 7};
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], cpus[i], cpus[i]);

  kstep_tick_repeat(500);
  for (int i = 1; i <= 3; i++)
    kstep_task_pin(tasks[i], 4, 6);
  kstep_tick_repeat(250);
}

static void on_tick(void) {
  pr_info(
      "nr_running: {\"cpu4\": %d, \"cpu5\": %d, \"cpu6\": %d, \"cpu7\": %d}\n",
      cpu_rq(4)->nr_running, cpu_rq(5)->nr_running, cpu_rq(6)->nr_running,
      cpu_rq(7)->nr_running);
}

KSTEP_DRIVER_DEFINE{
    .name = "extra_balance",
    .setup = setup,
    .run = run,
    .on_tick = on_tick,
    .step_interval_us = 1000,
    .print_load_balance = true,
};
