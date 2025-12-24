#include <linux/module.h>
#include <linux/reboot.h>

#include "kstep.h"

static char kstep_driver_name[32] = "default";
module_param_string(driver, kstep_driver_name, sizeof(kstep_driver_name), 0644);

struct kstep_driver *kstep_driver = NULL;

static int __init kstep_main(void) {
  kstep_write("/proc/sys/kernel/printk", "7", 1);
  ksym_init();

  TRACE_INFO("Initializing kSTEP with driver %s", kstep_driver_name);
  kstep_driver = kstep_driver_get(kstep_driver_name);
  kstep_driver_print(kstep_driver);

  // Isolate the CPUs to avoid interference
  kstep_prealloc_kworkers();
  kstep_disable_workqueue();
  kstep_move_kthreads();

  // Run userspace programs when we know the system is ready
  kstep_tasks_init();
  kstep_driver->setup();

  kstep_topo_print();

  // Control timer ticks and clock
  kstep_sched_timer_init();
  kstep_jiffies_init();
  kstep_sched_clock_init();

  // Reset the scheduler state to initial state
  kstep_reset_sched();

  if (kstep_driver->print_load_balance)
    kstep_trace_load_balance();
  if (kstep_driver->print_sched_softirq)
    kstep_trace_sched_softirq();

  // Enable printk time
  kstep_write("/sys/module/printk/parameters/time", "1", 1);

  TRACE_INFO("Running driver %s", kstep_driver->name);
  kstep_driver->run();
  TRACE_INFO("Exiting driver %s", kstep_driver->name);
  kernel_restart(NULL);

  return 0;
}
module_init(kstep_main);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("kSTEP");
