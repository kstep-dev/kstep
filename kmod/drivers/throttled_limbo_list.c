// https://github.com/torvalds/linux/commit/956dfda6a70885f18c0f8236a461aa2bc4f556ad
#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 0)
#define NUM_TASKS 3

static struct task_struct *tasks[NUM_TASKS];
static struct task_struct *helper;

static int count_limbo(struct cfs_rq *cfs_rq) {
  int n = 0;
  struct task_struct *p;
  list_for_each_entry(p, &cfs_rq->throttled_limbo_list, throttle_node) n++;
  return n;
}

static void setup(void) {
  for (int i = 0; i < NUM_TASKS; i++)
    tasks[i] = kstep_task_create();
  helper = kstep_task_create();
  kstep_cgroup_create("A");
  kstep_cgroup_create("A/B");
  kstep_cgroup_create("A/B/C");
}

static void run(void) {
  for (int i = 0; i < NUM_TASKS; i++) {
    kstep_task_pin(tasks[i], 1, 1);
    kstep_cgroup_add_task("A/B/C", tasks[i]->pid);
    kstep_task_wakeup(tasks[i]);
  }
  kstep_task_pin(helper, 2, 2);
  kstep_cgroup_add_task("A/B/C", helper->pid);

  // A: 5ms quota, 100ms period. C: 100ms quota, 100ms period.
  kstep_cgroup_write("A", "cpu.max", "5000 100000");
  kstep_cgroup_write("A/B/C", "cpu.max", "100000 100000");

  // Tick until tasks land on C's limbo list on CPU 1.
  // Sleeps give CPU 1 real time to process task work (return-to-user).
  struct cfs_rq *cfs_rq_c1 = tasks[0]->sched_task_group->cfs_rq[1];
  for (int i = 0; i < 200; i++) {
    kstep_tick();
    if (count_limbo(cfs_rq_c1) >= 2)
      break;
  }
  if (count_limbo(cfs_rq_c1) < 2)
    panic("need >= 2 limbo tasks, got %d", count_limbo(cfs_rq_c1));

  // Re-set C's quota to 1ms. On buggy kernel: runtime_remaining=0.
  kstep_cgroup_write("A/B/C", "cpu.max", "1000 100000");

  // Wake helper on CPU 2 to consume C's 1ms pool, then tick until
  // A's period timer fires (every 100 ticks), triggering unthrottle.
  // On buggy kernel: tg_unthrottle_up(C) → throttle → WARN.
  kstep_task_wakeup(helper);
  kstep_tick_repeat(120);
}
KSTEP_DRIVER_DEFINE{
    .name = "throttled_limbo_list",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
#endif
