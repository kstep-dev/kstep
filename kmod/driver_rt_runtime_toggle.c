// https://github.com/torvalds/linux/commit/9b58e976b3b391c0cf02e038d53dd0478ed3013c

#include "driver.h"

static struct task_struct *rt;

static void setup(void) { rt = kstep_task_create(); }

static void run(void) {
  kstep_sysctl_write("kernel.sched_rt_runtime_us", "%d", -1);
  kstep_task_fifo(rt);
  kstep_sysctl_write("kernel.sched_rt_runtime_us", "%d", 950000);
  kstep_tick_repeat(1501);
}

struct kstep_driver rt_runtime_toggle = {
    .name = "rt_runtime_toggle",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_tasks = true,
};
