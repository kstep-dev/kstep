#include "kstep.h"

static struct task_struct *busy_task;

static void setup(void) { busy_task = kstep_task_create(); }

static void run(void) {
  kstep_task_pin(busy_task, 1, 1);
  kstep_task_fork(busy_task, 20000);

  while (cpu_rq(1)->nr_running < 20000)
    kstep_sleep();

  kstep_tick_repeat(2000);
}

struct kstep_driver long_balance = {
    .name = "long_balance",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_sched_softirq = true,
};
