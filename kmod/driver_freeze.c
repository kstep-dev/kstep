#include "kstep.h"

static struct task_struct *tasks[3];

static void setup(void) {
  kstep_params.step_interval_us = 50000;

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
  kstep_task_sleep(p, 1);

  kstep_freeze_task(p);
  kstep_task_wakeup(p);

  kstep_tick_repeat(30);
}

struct kstep_driver freeze = {
    .name = "freeze",
    .setup = setup,
    .run = run,
};
