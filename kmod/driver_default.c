#include "kstep.h"

static struct task_struct *tasks[10];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++) 
    kstep_task_wakeup(tasks[i]);

  for (int i = 0; i < 15; i++) 
    kstep_tick();
}

struct kstep_driver default_driver = {
    .name = "default",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_rq = true,
    .print_tasks = true,
    .print_load_balance = true,
    .print_sched_softirq = true,
};
