// https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3

#include "driver.h"
#include "internal.h" // cpu_rq, sched_group_span

static struct task_struct *tasks[5];

static void setup(void) {
  kstep_on_tick_begin(kstep_output_nr_running);
  kstep_on_balance_selected(kstep_check_extra_balance);
  // SMT pairs: [1,2] [3,4], MC: [1-4]
  kstep_topo_set("SMT=1,2|3,4;CLS=1-2|3-4");

  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
}

static void run(void) {
  // nr_running on cpu 1-4: [1, 0, 3, 1]
  kstep_task_set_affinity(tasks[0], "1");
  kstep_task_set_affinity(tasks[1], "3");
  kstep_task_set_affinity(tasks[2], "3");
  kstep_task_set_affinity(tasks[3], "3");
  kstep_task_set_affinity(tasks[4], "4");
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    kstep_task_wakeup(tasks[i]);

  kstep_tick_repeat(500);
  for (int i = 1; i <= 3; i++)
    kstep_task_set_affinity(tasks[i], "1-3");
  kstep_tick_repeat(500);
}

KSTEP_DRIVER_DEFINE{
    .name = "extra_balance",
    .setup = setup,
    .run = run,
};
