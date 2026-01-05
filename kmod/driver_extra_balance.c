#include "driver.h"

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

struct kstep_driver extra_balance = {
    .name = "extra_balance",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_load_balance = true,
    .print_nr_running = true,
};
