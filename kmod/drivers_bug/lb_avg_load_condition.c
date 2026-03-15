// Reproducer for avg_load not computed in LB path for group_fully_busy.
//
// Bug: update_sg_lb_stats() only computes avg_load for group_overloaded,
// not group_fully_busy. But update_sd_pick_busiest() uses avg_load to
// compare group_fully_busy groups, making busiest selection arbitrary
// (always 0 vs 0) instead of load-aware.
//
// Setup: 3 CLS groups: A={1,2} (spare), B={3,4} (heavy), C={5,6} (light)
// Bug: C selected as busiest (last scanned, avg_load 0==0) -> light migrates
// Fix: B selected as busiest (higher avg_load) -> heavy migrates

#include <linux/version.h>

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)

static struct task_struct *heavy[2];
static struct task_struct *light[2];

static void setup(void) {
  kstep_topo_init();
  const char *cls[] = {"0", "1-2", "1-2", "3-4", "3-4", "5-6", "5-6"};
  kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  kstep_topo_apply();

  for (int i = 0; i < 2; i++) {
    heavy[i] = kstep_task_create();
    kstep_task_set_prio(heavy[i], -20);
  }

  for (int i = 0; i < 2; i++) {
    light[i] = kstep_task_create();
    kstep_task_set_prio(light[i], 19);
  }
}

static void run(void) {
  // Pin heavy tasks to Group B (CPUs 3-4)
  kstep_task_pin(heavy[0], 3, 3);
  kstep_task_pin(heavy[1], 4, 4);
  kstep_task_wakeup(heavy[0]);
  kstep_task_wakeup(heavy[1]);

  // Pin light tasks to Group C (CPUs 5-6)
  kstep_task_pin(light[0], 5, 5);
  kstep_task_pin(light[1], 6, 6);
  kstep_task_wakeup(light[0]);
  kstep_task_wakeup(light[1]);

  // Let PELT load averages stabilize
  kstep_tick_repeat(200);

  for (int i = 0; i < 2; i++) {
    TRACE_INFO("heavy[%d] pid=%d cpu=%d load_avg=%lu",
               i, heavy[i]->pid, task_cpu(heavy[i]),
               heavy[i]->se.avg.load_avg);
    TRACE_INFO("light[%d] pid=%d cpu=%d load_avg=%lu",
               i, light[i]->pid, task_cpu(light[i]),
               light[i]->se.avg.load_avg);
  }

  // Unpin all tasks to allow migration to CPUs 1-6
  for (int i = 0; i < 2; i++) {
    kstep_task_pin(heavy[i], 1, 6);
    kstep_task_pin(light[i], 1, 6);
  }

  // Let load balancer run
  kstep_tick_repeat(100);

  // Check which tasks ended up on Group A (CPUs 1-2)
  int heavy_on_a = 0, light_on_a = 0;
  for (int i = 0; i < 2; i++) {
    int hcpu = task_cpu(heavy[i]);
    int lcpu = task_cpu(light[i]);
    TRACE_INFO("result: heavy[%d] pid=%d cpu=%d", i, heavy[i]->pid, hcpu);
    TRACE_INFO("result: light[%d] pid=%d cpu=%d", i, light[i]->pid, lcpu);
    if (hcpu >= 1 && hcpu <= 2)
      heavy_on_a++;
    if (lcpu >= 1 && lcpu <= 2)
      light_on_a++;
  }

  TRACE_INFO("heavy_on_groupA=%d light_on_groupA=%d", heavy_on_a, light_on_a);

  if (light_on_a > 0 && heavy_on_a == 0) {
    kstep_fail("avg_load bug: light task migrated to spare group "
               "(wrong busiest selection), heavy_on_A=%d light_on_A=%d",
               heavy_on_a, light_on_a);
  } else if (heavy_on_a > 0) {
    kstep_pass("correct busiest: heavy task migrated to spare group, "
               "heavy_on_A=%d light_on_A=%d", heavy_on_a, light_on_a);
  } else {
    kstep_fail("no migration occurred, heavy_on_A=%d light_on_A=%d",
               heavy_on_a, light_on_a);
  }

  kstep_tick_repeat(10);
}

KSTEP_DRIVER_DEFINE{
    .name = "lb_avg_load_condition",
    .setup = setup,
    .run = run,
    .step_interval_us = 1000,
};

#endif
