#include "driver.h"
#include "internal.h" // cpu_rq

static struct task_struct *tasks[4];

static void setup(void) {
  kstep_cpu_set_capacity(1, SCHED_CAPACITY_SCALE);
  kstep_cpu_set_capacity(2, SCHED_CAPACITY_SCALE / 2);
  kstep_cpu_set_capacity(3, SCHED_CAPACITY_SCALE);
  kstep_cpu_set_capacity(4, SCHED_CAPACITY_SCALE / 2);

  kstep_topo_init();
  const char *cls[] = {"0", "1-2", "1-2", "3-4", "3-4"};
  kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  kstep_topo_apply();

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // making the nr_running on cpu 1-4 to [1, 0, 2, 1]
  kstep_task_kernel_pin(tasks[0], 1, 1);
  kstep_task_kernel_pin(tasks[1], 3, 3);
  kstep_task_kernel_pin(tasks[2], 3, 3);
  kstep_task_kernel_pin(tasks[3], 4, 4);
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_kernel_wakeup(tasks[i]);

  kstep_tick_repeat(50);
  kstep_task_kernel_pin(tasks[1], 2, 3);
  kstep_task_kernel_pin(tasks[2], 2, 3);
  kstep_tick_repeat(250);
}


KSTEP_DRIVER_DEFINE{
    .name = "even_idle_cpu",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .on_sched_balance_selected = kstep_output_balance,
    .step_interval_us = 100,
};
