#include <linux/module.h>
#include <linux/reboot.h>

#include "kstep.h"

struct kstep_params_t kstep_params = {
    .driver = "noop",
    .step_interval_us = 10000,
    .special_topo = false,
    .print_rq_stats = true,
    .print_tasks = true,
    .print_nr_running = false,
};
module_param_string(driver, kstep_params.driver, sizeof(kstep_params.driver),
                    0644);
module_param_named(step_interval_us, kstep_params.step_interval_us, ullong,
                   0644);
module_param_named(special_topo, kstep_params.special_topo, bool, 0644);
module_param_named(print_tasks, kstep_params.print_tasks, bool, 0644);
module_param_named(print_nr_running, kstep_params.print_nr_running, bool, 0644);

void kstep_params_print(void) {
  TRACE_INFO("kSTEP params:");
  TRACE_INFO("- driver: %s", kstep_params.driver);
  TRACE_INFO("- step_interval_us: %llu", kstep_params.step_interval_us);
  TRACE_INFO("- special_topo: %d", kstep_params.special_topo);
  TRACE_INFO("- print_rq_stats: %d", kstep_params.print_rq_stats);
  TRACE_INFO("- print_tasks: %d", kstep_params.print_tasks);
  TRACE_INFO("- print_nr_running: %d", kstep_params.print_nr_running);
}

static int __init kstep_main(void) {
  kstep_write("/proc/sys/kernel/printk", "7", 1);
  TRACE_INFO("Initializing kSTEP");
  ksym_init();
  struct kstep_driver *driver = kstep_driver_get(kstep_params.driver);
  if (driver->pre_init)
    driver->pre_init();

  kstep_params_print();
  if (kstep_params.special_topo)
    kstep_topo_use_special();
  kstep_topo_print();

  // Isolate the CPUs to avoid interference
  kstep_prealloc_kworkers();
  kstep_disable_workqueue();
  kstep_move_kthreads();
  kstep_tasks_init();

  // Run userspace programs when we know the system is ready
  if (driver->init)
    driver->init();

  // Control timer ticks and clock
  kstep_tick_init();

  // Reset the scheduler state to initial state
  kstep_reset_sched();

  if (kstep_params.print_load_balance)
    kstep_trace_load_balance();
  if (kstep_params.print_sched_softirq)
    kstep_trace_sched_softirq();

  // Enable printk time
  kstep_write("/sys/module/printk/parameters/time", "1", 1);

  TRACE_INFO("Running driver %s", driver->name);
  driver->body();
  TRACE_INFO("Exiting driver %s", driver->name);
  kernel_restart(NULL);
  
  return 0;
}
module_init(kstep_main);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("kSTEP");
