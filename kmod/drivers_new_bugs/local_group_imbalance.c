// Replay driver for data/fuzz/crashes/work conserving/work conserving_20260325_201209_w0
#include "driver.h"

static struct task_struct *tasks[6];
static void setup(void) {
  kstep_topo_init();
  {
    const char *cls[] = {"0", "1-2", "1-2", "3-4", "3-4"};
    kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  }
  kstep_topo_apply();
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
  for (int i = 0; i < 3; i++)
    kstep_task_pin(tasks[i], 4, 4);
  for (int i = 3; i < 6; i++)
    kstep_task_pin(tasks[i], 1, 2);
}

static void run(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  kstep_tick_repeat(10);

  for (int i = 3; i < 6; i++)
    kstep_task_pin(tasks[i], 1, 4);

  kstep_tick_repeat(400);
  
}

KSTEP_DRIVER_DEFINE{
    .name = "local_group_imbalance",
    .setup = setup,
    .run = run,
    .on_tick_end = kstep_output_nr_running,
    .on_sched_balance_selected = kstep_output_balance,
    .step_interval_us = 10000,
};
