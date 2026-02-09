#include "driver.h"

static struct task_struct *tasks[10];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  for (int i = 0; i < 5; i++)
    kstep_tick();
}

KSTEP_DRIVER_DEFINE{
    .name = "default",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_tasks = true,
    .print_load_balance = true,
    .print_sched_debug = true,
};
