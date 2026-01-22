#include <linux/sched/signal.h>

#include "driver.h"

#define NUM_TASKS 20000

static struct task_struct *busy_task;

static void setup(void) { busy_task = kstep_task_create(); }

static void *fork_finished(void) {
  struct task_struct *p;
  int nr_running = 0;
  for_each_process(p) {
    if (task_cpu(p) == task_cpu(busy_task) && p->parent == busy_task)
      nr_running++;
  }
  return nr_running >= NUM_TASKS ? (void *)true : (void *)false;
}

static void run(void) {
  kstep_task_pin(busy_task, 1, 1);
  kstep_task_fork(busy_task, NUM_TASKS);
  kstep_sleep_until(fork_finished);
  kstep_tick_repeat(3000);
}

struct kstep_driver long_balance = {
    .name = "long_balance",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .print_sched_softirq = true,
};
