#include "driver.h"
#include "internal.h"
#include "linux/printk.h"
#include "op_handler.h"

static struct task_struct *tasks[10];

static void setup(void) {
  kstep_cov_init();
}

static void run(void) {
  kstep_cov_enable();
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();

  for (int i = 0; i < ARRAY_SIZE(tasks); i++) {
    kstep_task_kernel_wakeup(tasks[i]);
    kstep_cov_dump();
    kstep_cov_cmd_id_inc();
    pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", OP_TASK_WAKEUP, tasks[i]->pid, 0, 0);
  }

  // kstep_cgroup_add_task("", tasks[0]->pid);

  kstep_cgroup_create("g0");

  
  for (int i = 0; i < 5; i++) {
    kstep_tick();
    kstep_cov_dump();
    kstep_cov_cmd_id_inc();
    pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", OP_TICK, 0, 0, 0);
  }

  kstep_cgroup_add_task("g0", tasks[0]->pid);
  kstep_cov_dump();
  kstep_cov_cmd_id_inc();
  pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", OP_CGROUP_ADD_TASK, tasks[0]->pid, 0, 0);

  kstep_cgroup_add_task("", tasks[0]->pid);
  kstep_cov_dump();
  kstep_cov_cmd_id_inc();
  pr_info("EXECOP: {\"op\": %d, \"a\": %d, \"b\": %d, \"c\": %d}\n", OP_CGROUP_ADD_TASK, tasks[0]->pid, 0, 0);

  kstep_cov_disable();

  kstep_cov_dump();
}

KSTEP_DRIVER_DEFINE{
    .name = "default",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .on_tick_begin = kstep_output_curr_task,
};
