// https://github.com/torvalds/linux/commit/5068d84054b766efe7c6202fc71b2350d1c326f1

#include "driver.h"
#include "internal.h"

static struct task_struct *target_task;
static struct task_struct *other_task;

static void setup(void) {
  // Create target task and add it to group g0
  target_task = kstep_task_create();
  kstep_cgroup_create("g0");
  kstep_cgroup_add_task("g0", target_task->pid);

  // Create other tasks
  other_task = kstep_task_create();
}

static void run(void) {
  kstep_task_set_prio(other_task, 19);
  kstep_task_wakeup(other_task);

  kstep_task_wakeup(target_task);

  kstep_tick_repeat(5);

  kstep_cgroup_set_weight("g0", 10000);

  kstep_tick_repeat(5);
}

static void on_tick_begin(void) {
  struct rq *rq = cpu_rq(1);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
  s64 min_vruntime = rq->cfs.zero_vruntime - INIT_TIME_NS;
#else
  s64 min_vruntime = rq->cfs.min_vruntime - INIT_TIME_NS;
#endif
  kstep_json_print_2kv("type", "min_vruntime", "val", "%lld", min_vruntime);
}

KSTEP_DRIVER_DEFINE{
    .name = "lag_vruntime",
    .setup = setup,
    .run = run,
    .on_tick_begin = on_tick_begin,
    .step_interval_us = 1000,
};
