// https://github.com/torvalds/linux/commit/111688ca1c4a
//
// Bug: In calculate_imbalance(), when local group is group_fully_busy and
// local->avg_load >= busiest->avg_load, the imbalance formula
//   min((busiest->avg_load - sds->avg_load) * busiest->group_capacity,
//       (sds->avg_load - local->avg_load) * local->group_capacity)
// underflows (unsigned subtraction), producing a huge imbalance value.
// This causes the scheduler to incorrectly pull tasks from a less-loaded
// busiest group to the more-loaded local group.
//
// Fix: Before the formula, check if local->avg_load >= busiest->avg_load
// and return imbalance=0 if so.
//
// Reproduce: Use asymmetric CPU capacities to create the condition.
// CPU 1 (low capacity=256) with 1 task → group_fully_busy, avg_load ~4096
// CPU 2 (full capacity=1024) with 3 tasks → group_overloaded, avg_load ~3072
// Since local->avg_load > busiest->avg_load, the bug triggers.
// One migratable task on CPU 2 gets wrongly pulled to CPU 1 on buggy kernel.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 6, 0)

static struct task_struct *task_local;
static struct task_struct *tasks_busiest[2];
static struct task_struct *task_migrator;

static void setup(void) {
  // CPU 1: low capacity → high avg_load per task
  // CPU 2: full capacity → lower avg_load per task
  kstep_cpu_set_capacity(1, SCHED_CAPACITY_SCALE / 4);
  kstep_cpu_set_capacity(2, SCHED_CAPACITY_SCALE);

  // Separate clusters: {0}, {1}, {2}
  kstep_topo_init();
  const char *cls[] = {"0", "1", "2"};
  kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  kstep_topo_apply();

  task_local = kstep_task_create();
  for (int i = 0; i < 2; i++)
    tasks_busiest[i] = kstep_task_create();
  task_migrator = kstep_task_create();
}

static void run(void) {
  // CPU 1: 1 task → group_fully_busy
  // avg_load = group_load * 1024 / 256 ≈ 4096
  kstep_task_pin(task_local, 1, 1);

  // CPU 2: 3 tasks → group_overloaded (nr_running=3 > group_weight=1)
  // avg_load = 3 * group_load * 1024 / 1024 ≈ 3072
  for (int i = 0; i < 2; i++)
    kstep_task_pin(tasks_busiest[i], 2, 2);
  kstep_task_pin(task_migrator, 2, 2);

  // Let PELT load averages build up
  kstep_tick_repeat(300);

  TRACE_INFO("Before: CPU1 nr=%d, CPU2 nr=%d",
             cpu_rq(1)->nr_running, cpu_rq(2)->nr_running);

  // Allow task_migrator to run on both CPUs
  kstep_task_pin(task_migrator, 1, 2);

  // Trigger load balancing
  kstep_tick_repeat(300);

  int cpu1_nr = cpu_rq(1)->nr_running;
  int cpu2_nr = cpu_rq(2)->nr_running;

  TRACE_INFO("After: CPU1 nr=%d, CPU2 nr=%d, migrator_cpu=%d",
             cpu1_nr, cpu2_nr, task_cpu(task_migrator));

  if (cpu1_nr > 1)
    kstep_fail("negative imbalance: task pulled to more-loaded local "
               "(CPU1 nr=%d)",
               cpu1_nr);
  else
    kstep_pass("no negative imbalance: CPU1 nr=%d, CPU2 nr=%d",
               cpu1_nr, cpu2_nr);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "neg_imbalance",
    .setup = setup,
    .run = run,
    .on_tick_begin = kstep_output_nr_running,
    .on_sched_balance_selected = kstep_output_balance,
    .step_interval_us = 100,
};
