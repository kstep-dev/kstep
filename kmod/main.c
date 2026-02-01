#include <linux/module.h>
#include <linux/reboot.h>

#include "internal.h"

struct kstep_driver *kstep_driver = NULL;

static char driver_name[DRIVER_NAME_LEN] = "default";
module_param_string(driver, driver_name, DRIVER_NAME_LEN, 0644);

static int __init kstep_main(void) {
  kstep_sysctl_write("kernel.printk", "%d", 7);
  kstep_driver = kstep_sym_init(driver_name);
  kstep_driver_print(kstep_driver);

  // Isolate the CPUs to avoid interference
  kstep_prealloc_kworkers();
  kstep_disable_workqueue();
  kstep_move_kthreads();

  // Run userspace programs when we know the system is ready
  kstep_driver->setup();
  kstep_topo_print();

  // Control timer ticks and clock
  kstep_sched_timer_init();
  kstep_jiffies_init();
  kstep_sched_clock_init();

  // Reset the scheduler state to initial state
  kstep_reset_runqueues();
  kstep_reset_cpumask();
  kstep_reset_tasks();
  kstep_patch_min_vruntime();

  if (kstep_driver->print_load_balance)
    kstep_trace_load_balance();

  // Enable printk time
  kstep_write("/sys/module/printk/parameters/time", "1", 1);

  TRACE_INFO("Running driver %s", kstep_driver->name);

  // Enable auto trace of runqueue
  if (kstep_driver->auto_trace)
    kstep_rq_trace_init();

  kstep_driver->run();
  TRACE_INFO("Exiting driver %s", kstep_driver->name);
  kernel_restart(NULL);

  return 0;
}
module_init(kstep_main);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("kSTEP");
