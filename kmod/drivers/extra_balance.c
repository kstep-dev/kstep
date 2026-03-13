// https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3

#include "driver.h"
#include "internal.h" // cpu_rq

static struct task_struct *tasks[5];

static void setup(void) {
  // SMT pairs: [1,2] [3,4], MC: [1-4]
  kstep_topo_init();
  const char *cpulists[] = {"0", "1-2", "1-2", "3-4", "3-4"};
  kstep_topo_set_smt(cpulists, ARRAY_SIZE(cpulists));
  kstep_topo_set_cls(cpulists, ARRAY_SIZE(cpulists));
  kstep_topo_apply();

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // nr_running on cpu 1-4: [1, 0, 3, 1]
  kstep_task_pin(tasks[0], 1, 1);
  kstep_task_pin(tasks[1], 3, 3);
  kstep_task_pin(tasks[2], 3, 3);
  kstep_task_pin(tasks[3], 3, 3);
  kstep_task_pin(tasks[4], 4, 4);
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  kstep_tick_repeat(500);
  for (int i = 1; i <= 3; i++)
    kstep_task_pin(tasks[i], 1, 3);
  kstep_tick_repeat(500);
}


KSTEP_DRIVER_DEFINE{
    .name = "extra_balance",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .on_sched_balance_selected = kstep_output_balance,
    .step_interval_us = 100,
};
