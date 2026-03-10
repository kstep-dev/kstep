// https://github.com/torvalds/linux/commit/111688ca1c4a
//
// Bug: In calculate_imbalance(), when local group is group_fully_busy and
// the local group's avg_load exceeds busiest group's avg_load, the formula
// (sds->avg_load - local->avg_load) * local->group_capacity wraps to a
// huge unsigned value, causing incorrect migration decisions.
//
// Fix: Add a check: if local->avg_load >= busiest->avg_load, set
// imbalance = 0 and return early.
//
// Reproduce: Create two clusters with asymmetric task weight. Local cluster
// (CPU 1) has 1 heavy task (nice -20, high weight → high avg_load) and is
// group_fully_busy. Busiest cluster (CPUs 2-3) has 3 light tasks (nice 0)
// and is group_overloaded. When the wrapped imbalance is computed, the
// scheduler erroneously pulls tasks from busiest to the more-loaded local.

#include "driver.h"
#include "internal.h"
#include <linux/version.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 6, 0)

static struct task_struct *heavy_task;
static struct task_struct *light_tasks[3];

static void setup(void) {
  // Two clusters: CPU 1 (local), CPUs 2-3 (busiest)
  kstep_topo_init();
  const char *cls[] = {"0", "1", "2-3", "2-3"};
  kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  kstep_topo_apply();

  heavy_task = kstep_task_create();
  for (int i = 0; i < 3; i++)
    light_tasks[i] = kstep_task_create();
}

static void run(void) {
  // Prevent jiffies drift on 5.6: set last_jiffies_update far in the future
  // so tick_do_update_jiffies64() always returns early.
  KSYM_IMPORT_TYPED(ktime_t, last_jiffies_update);
  *KSYM_last_jiffies_update = KTIME_MAX;

  // Local cluster (CPU 1): 1 heavy task → group_fully_busy, high avg_load
  kstep_task_pin(heavy_task, 1, 1);
  kstep_task_set_prio(heavy_task, -20);

  // Busiest cluster (CPUs 2-3): 3 light tasks → group_overloaded
  kstep_task_pin(light_tasks[0], 2, 2);
  kstep_task_pin(light_tasks[1], 2, 2);
  kstep_task_pin(light_tasks[2], 3, 3);

  // Let loads settle so PELT catches up to the actual weights
  kstep_tick_repeat(200);

  TRACE_INFO("Before unpin: light task CPUs: %d %d %d",
             task_cpu(light_tasks[0]), task_cpu(light_tasks[1]),
             task_cpu(light_tasks[2]));

  // Unpin light tasks to allow cross-cluster migration
  for (int i = 0; i < 3; i++)
    kstep_task_pin(light_tasks[i], 1, 3);

  // Tick more to trigger load balancing from CPU 1's perspective
  kstep_tick_repeat(300);

  int wrong_migrations = 0;
  for (int i = 0; i < 3; i++) {
    int cpu = task_cpu(light_tasks[i]);
    TRACE_INFO("light_tasks[%d] on CPU %d", i, cpu);
    if (cpu == 1)
      wrong_migrations++;
  }

  if (wrong_migrations > 0) {
    kstep_fail("negative imbalance: %d task(s) pulled from "
               "less-loaded busiest to more-loaded local group",
               wrong_migrations);
  } else {
    kstep_pass("no wrong migration: imbalance calculation correct");
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "neg_imbalance",
    .setup = setup,
    .run = run,
    .step_interval_us = 100,
    .tick_interval_ns = 1000000,
};
