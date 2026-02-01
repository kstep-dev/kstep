// https://github.com/torvalds/linux/commit/cd9626e9ebc77edec33023fe95dab4b04ffc819d

#include "driver.h"

static struct task_struct *tasks[3];

static void setup(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void *is_ineligible(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    if (tasks[i]->on_cpu && !kstep_eligible(&tasks[i]->se))
      return tasks[i];
  return NULL;
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  struct task_struct *p = kstep_tick_until(is_ineligible);
  kstep_task_usleep(p, 20000); // step_interval_us * 2

  kstep_freeze_task(p);
  kstep_task_wakeup(p);

  kstep_tick_repeat(30);
}

struct kstep_driver freeze = {
    .name = "freeze",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_tasks = true,
    .print_rq = true,
};
