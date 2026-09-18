#include "driver.h"

static struct task_struct *tasks[4];

static void setup(void) {
  kstep_on_tick_begin(kstep_output_nr_running);
  kstep_on_balance_selected(kstep_output_balance);
  kstep_topo_set("CLS=1-2|3-4;CAP=2,4:512");

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // making the nr_running on cpu 1-4 to [1, 0, 2, 1]
  kstep_task_set_affinity(tasks[0], "1");
  kstep_task_set_affinity(tasks[1], "3");
  kstep_task_set_affinity(tasks[2], "3");
  kstep_task_set_affinity(tasks[3], "4");
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  kstep_tick_repeat(50);
  kstep_task_set_affinity(tasks[1], "2-3");
  kstep_task_set_affinity(tasks[2], "2-3");
  kstep_tick_repeat(250);
}


KSTEP_DRIVER_DEFINE{
    .name = "even_idle_cpu",
    .setup = setup,
    .run = run,
};
