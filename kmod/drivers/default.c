#include "driver.h"

static struct task_struct *tasks[10];

static void setup(void) {}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();

  for (int i = 0; i < ARRAY_SIZE(tasks); i++) {
    kstep_task_wakeup(tasks[i]);
  }

  kstep_cgroup_create("g0");

  for (int i = 0; i < 5; i++) {
    kstep_tick();
  }

  kstep_cgroup_add_task("g0", tasks[0]->pid);

  kstep_cgroup_add_task("", tasks[0]->pid);
}

static void on_tick_begin(void) {
  kstep_output_curr_task();
  kstep_print_sched_debug();
}

KSTEP_DRIVER_DEFINE{
    .name = "default",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .on_tick_begin = on_tick_begin,
};
