#include "driver.h"

static struct task_struct *tasks[10];

static void setup(void) {}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();

  for (int i = 0; i < ARRAY_SIZE(tasks); i++) {
    kstep_task_wakeup(tasks[i]);
  }

  kstep_print_sched_debug();
  kstep_tick();
  kstep_print_sched_debug();

  for (int i = 0; i < 100; i++) {
    kstep_tick();
    for (int j = 0; j < ARRAY_SIZE(tasks); j++) {
      if (tasks[j]->on_cpu) printk("%d", tasks[j]->pid);
    }
  }

  TRACE_INFO("Done");
}

KSTEP_DRIVER_DEFINE{
    .name = "default",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
