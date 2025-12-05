#include "kstep.h"

static struct task_struct *find_last_task(void) {
  struct task_struct *p;
  struct task_struct *target;
  for_each_process(p) {
    if (strcmp(p->comm, busy_task->comm) != 0)
      continue;
    target = p;
  }
  return target;
}

static void controller_body(void) {
  kstep_cgroup_create("l1_0", "2");

  for (int i = 0; i < 3; i++) {
    kstep_task_fork_pin(busy_task, 1, 2, 2);
    struct task_struct *pin_task = find_last_task();
    kstep_task_signal(pin_task, SIGCODE_REWEIGHT, 19, 0, 0);
  }

  kstep_task_signal(busy_task, SIGCODE_CLONE3_L1_0, 1, 0, 0);
  struct task_struct *l1_0_task = find_last_task();
  kstep_tick_repeat(15);
  kstep_cgroup_write_file("l1_0", "cpu.weight", "10000");
  kstep_tick_repeat(5);
}

struct controller_ops controller_lag_vruntime = {
    .name = "lag_vruntime",
    .body = controller_body,
};
