#include "kstep.h"

static void pre_init(void) {
  kstep_params.print_lb_events = true;
  kstep_params.print_nr_running = true;
  kstep_params.print_tasks = false;
  kstep_params.print_rq_stats = false;
  kstep_params.step_interval_us = 1000;
}

static struct task_struct *tasks[5];

static void init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void body(void) {
  // making the nr_running on cpu 4-7 to [1, 0, 3, 1]
  int cpus[] = {4, 6, 6, 6, 7};
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_pin(tasks[i], cpus[i], cpus[i]);

  kstep_tick_repeat(1000);
  for (int i = 1; i <= 3; i++)
    kstep_task_pin(tasks[i], 4, 6);
  kstep_tick_repeat(301);
}

struct kstep_driver extra_balance = {
    .name = "extra_balance",
    .pre_init = pre_init,
    .init = init,
    .body = body,
};
