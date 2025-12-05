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

static struct task_struct *tasks[4];
static void controller_init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
  kstep_cgroup_create("l1_0", "2");
  kstep_cgroup_write("l1_0", "cgroup.procs", "%d", tasks[3]->pid);
}

static void controller_body(void) {
  for (int i = 0; i < 3; i++) {
    kstep_task_pin(tasks[i], 2, 2);
    kstep_task_reweight(tasks[i], 19);
  }
  kstep_task_wakeup(tasks[3]);

  kstep_tick_repeat(15);
  kstep_cgroup_write("l1_0", "cpu.weight", "%d", 10000);
  kstep_tick_repeat(5);
}

struct controller_ops controller_lag_vruntime = {
    .name = "lag_vruntime",
    .init = controller_init,
    .body = controller_body,
};
