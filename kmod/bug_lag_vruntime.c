#include "kstep.h"

#define TARGET_TASK "test-proc"

static struct task_struct *find_last_task(void) {
  struct task_struct *p;
  struct task_struct *target;
  for_each_process(p) {
    if (strcmp(p->comm, TARGET_TASK) != 0)
      continue;
    target = p;
  }
  return target;
}

static void controller_body(void) {
  kstep_cgroup_create("l1_0", "2");

  for (int i = 0; i < 3; i++) {
    send_sigcode2(busy_task, SIGCODE_FORK_PIN, 1, 2);
    struct task_struct *pin_task = find_last_task();
    send_sigcode(pin_task, SIGCODE_REWEIGHT, 19);
  }

  send_sigcode(busy_task, SIGCODE_CLONE3_L1_0, 1);
  struct task_struct *l1_0_task = find_last_task();
  kstep_tick_repeat(20);
  kstep_cgroup_write_file("l1_0", "cpu.weight", "10000");
  kstep_tick_repeat(10);
}

struct controller_ops controller_lag_vruntime = {
    .name = "lag_vruntime",
    .body = controller_body,
};
