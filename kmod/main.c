#include <generated/utsrelease.h>
#include <linux/module.h>
#include <linux/reboot.h>

#include "driver.h"
#include "internal.h"

struct kstep_driver *kstep_driver = NULL;

static char driver_name[DRIVER_NAME_LEN] = "default";
module_param_string(driver, driver_name, DRIVER_NAME_LEN, 0644);

static int __init kstep_main(void) {
  kstep_output_init();

  TRACE_INFO("Starting %s on Linux %s", driver_name, UTS_RELEASE);
  kstep_driver = kstep_sym_init(driver_name);

  // Isolate the CPUs to avoid interference
  kstep_prealloc_kworkers();
  kstep_disable_workqueue();
  kstep_move_kthreads();

  // Run userspace programs when we know the system is ready
  kstep_task_init();
  kstep_cgroup_init();
  kstep_trace_sched_group_alloc(); // also sets min_vruntime
  kstep_driver->setup();
  kstep_topo_print();

  // Control timer ticks and clock
  kstep_tick_init();
  kstep_jiffies_init();
  kstep_sched_clock_init();

  // Reset the scheduler state to initial state
  kstep_reset_runqueues();
  kstep_reset_cpumask();
  kstep_reset_tasks();

  TRACE_INFO("Running driver %s", kstep_driver->name);

  if (kstep_driver->on_sched_balance_begin ||
      kstep_driver->on_sched_balance_selected)
    kstep_trace_sched_balance_begin();
  if (kstep_driver->on_sched_balance_selected)
    kstep_trace_sched_balance_selected();
  kstep_driver->run();

  TRACE_INFO("Exiting driver %s on Linux %s", kstep_driver->name, UTS_RELEASE);

  kernel_restart(NULL);

  return 0;
}
module_init(kstep_main);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("kSTEP");
