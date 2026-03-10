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
// Reproduce: Use task weight asymmetry. CPU 1 runs 1 heavy task (nice -20,
// weight=88761) making it group_fully_busy with very high avg_load. CPU 2
// runs 3 light tasks (nice 0, weight=1024) making it group_overloaded but
// with much lower avg_load. The unsigned subtraction wraps, causing a wrong
// migration from CPU 2 to CPU 1.

#include "driver.h"
#include "internal.h"

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 6, 0)

static struct task_struct *heavy_task;
static struct task_struct *light_tasks[2];
static struct task_struct *task_migrator;

static void setup(void) {
  // Separate clusters: {0}, {1}, {2}
  kstep_topo_init();
  const char *cls[] = {"0", "1", "2"};
  kstep_topo_set_cls(cls, ARRAY_SIZE(cls));
  kstep_topo_apply();

  heavy_task = kstep_task_create();
  for (int i = 0; i < 2; i++)
    light_tasks[i] = kstep_task_create();
  task_migrator = kstep_task_create();
}

static void run(void) {
  // CPU 1: 1 heavy task (nice -20, weight=88761) → group_fully_busy
  // avg_load ≈ 88761 (much higher than CPU 2)
  kstep_task_pin(heavy_task, 1, 1);
  kstep_task_set_prio(heavy_task, -20);

  // CPU 2: 3 light tasks (nice 0, weight=1024) → group_overloaded
  // avg_load ≈ 3072 (much lower than CPU 1)
  for (int i = 0; i < 2; i++)
    kstep_task_pin(light_tasks[i], 2, 2);
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
