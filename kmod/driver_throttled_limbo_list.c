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

  // A: small quota (5ms, 100ms period). C: large quota to avoid self-throttle.
  kstep_cgroup_write("A", "cpu.max", "5000 100000");
  kstep_cgroup_write("A/B/C", "cpu.max", "100000 100000");

  // Tick until tasks land on C's limbo list on CPU 1.
  struct cfs_rq *cfs_rq_c1 = tasks[0]->sched_task_group->cfs_rq[1];
  for (int i = 0; i < 50; i++) {
    kstep_tick();
    kstep_sleep();
    if (count_limbo(cfs_rq_c1) >= 2)
      break;
  }
  int nlimbo = count_limbo(cfs_rq_c1);
  if (nlimbo < 2) {
    kstep_fail("need >= 2 limbo tasks, got %d", nlimbo);
    return;
  }

  // Re-set C's quota to 1ms with 100ms period. On buggy kernel this
  // sets runtime_remaining = 0; the fix sets it to 1.
  kstep_cgroup_write("A/B/C", "cpu.max", "1000 100000");

  // The helper task is on CPU 2's runqueue. Tick+sleep in a loop to
  // (a) give CPU 2 time to schedule the helper via IPI-triggered
  // reschedule and (b) force update_curr to drain C's pool.
  struct cfs_bandwidth *cfs_b = &tasks[0]->sched_task_group->cfs_bandwidth;
  for (int i = 0; i < 20 && cfs_b->runtime > 0; i++) {
    for (int j = 0; j < 20; j++)
      kstep_sleep();
    kstep_tick();
  }

  // Wait for A's period timer to fire (~100ms real time) and
  // async-unthrottle A on CPU 1 via CSD. Then tick to deliver the
  // CSD IPI. On buggy kernel: tg_unthrottle_up(C) enqueues the first
  // limbo task → runtime_remaining=0, pool=0 → throttle → WARN.
  for (int i = 0; i < 300; i++)
    kstep_sleep();
  kstep_tick_repeat(3);
  for (int i = 0; i < 10; i++)
    kstep_sleep();
}
#else
static void setup(void) { panic("unsupported kernel version"); }
static void run(void) {}
#endif

KSTEP_DRIVER_DEFINE{
    .name = "throttled_limbo_list",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};
