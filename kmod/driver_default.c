#include "driver.h"
#include "internal.h"

static struct task_struct *tasks[10];

static void setup(void) {
  kstep_cov_init();
}

static void run(void) {
  kstep_cov_enable();
  // A sequence of operations for debugging
  tasks[0] = kstep_task_create();
  kstep_tick_repeat(2);
  kstep_tick();
  kstep_cgroup_create("cgroot");
  kstep_cgroup_create("cgroot/cg0");
  kstep_cgroup_set_cpuset("cgroot/cg0", "2-9");
  tasks[1] = kstep_task_create();
  kstep_tick();
  kstep_tick_repeat(1);
  kstep_cgroup_set_weight("cgroot/cg0", 4364);
  kstep_cgroup_add_task("cgroot/cg0", tasks[0]->pid);
  kstep_task_wakeup(tasks[0]);
  tasks[2] = kstep_task_create();
  tasks[3] = kstep_task_create();
  kstep_cgroup_set_cpuset("cgroot/cg0", "7-9");
  kstep_cgroup_add_task("cgroot/cg0", tasks[1]->pid);
  kstep_tick_repeat(4);
  kstep_cov_disable();
  kstep_cov_dump();
  TRACE_INFO("Driver execution completed");
}

KSTEP_DRIVER_DEFINE{
    .name = "default",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
    .print_tasks = true,
    // .print_load_balance = true,
    // .print_sched_debug = true,
};
