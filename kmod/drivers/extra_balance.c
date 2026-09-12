// https://github.com/torvalds/linux/commit/6d7e4782bcf549221b4ccfffec2cf4d1a473f1a3

#include "driver.h"
#include "internal.h" // cpu_rq, sched_group_span

static struct task_struct *tasks[5];

// Log the balance and flag one on a busy CPU while a CPU of its own group idles (the bug).
static void check_extra_balance(int cpu, struct sched_domain *sd) {
  struct sched_group *sg = sd->groups;
  int i;

  kstep_output_balance(cpu, sd);
  if (cpu_rq(cpu)->nr_running == 0)
    return;
  do {
    if (!cpumask_test_cpu(cpu, sched_group_span(sg)))
      continue;
    for_each_cpu(i, sched_group_span(sg))
      if (cpu_rq(i)->nr_running == 0) {
        pr_info("warn: load balance triggered on busy cpu while idle cpu in the same group");
        return;
      }
  } while (sg != sd->groups);
}

static void setup(void) {
  // SMT pairs: [1,2] [3,4], MC: [1-4]
  kstep_topo_set("SMT=0|1-2|3-4;CLS=0|1-2|3-4");

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
    .on_sched_balance_selected = check_extra_balance,
};
