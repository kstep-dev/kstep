#include "kstep.h"

static struct task_struct *tasks[4];

static void init(void) {
  for (int i = 0; i < ARRAY_SIZE(tasks); i++)
    tasks[i] = kstep_task_create();
  kstep_cgroup_create("l1_0");
  kstep_cgroup_write("l1_0", "cpuset.cpus", "2");
  kstep_cgroup_write("l1_0", "cgroup.procs", "%d", tasks[3]->pid);
}

static void body(void) {
  for (int i = 0; i < 3; i++) {
    kstep_task_pin(tasks[i], 2, 2);
    kstep_task_reweight(tasks[i], 19);
  }
  kstep_task_wakeup(tasks[3]);
  kstep_tick_repeat(10);
  kstep_cgroup_write("l1_0", "cpu.weight", "%d", 10000);
  kstep_tick_repeat(5);
}

struct kstep_driver lag_vruntime = {
    .name = "lag_vruntime",
    .init = init,
    .body = body,
};
