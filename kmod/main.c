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
module_param_named(print_lb_events, kstep_params.print_lb_events, bool, 0644);

void kstep_params_print(void) {
  TRACE_INFO("kSTEP params:");
  TRACE_INFO("- driver: %s", kstep_params.driver);
  TRACE_INFO("- step_interval_us: %llu", kstep_params.step_interval_us);
  TRACE_INFO("- special_topo: %d", kstep_params.special_topo);
  TRACE_INFO("- print_rq_stats: %d", kstep_params.print_rq_stats);
  TRACE_INFO("- print_tasks: %d", kstep_params.print_tasks);
  TRACE_INFO("- print_nr_running: %d", kstep_params.print_nr_running);
  TRACE_INFO("- print_lb_events: %d", kstep_params.print_lb_events);
}

static int __init kstep_main(void) {
  TRACE_INFO("Initializing kSTEP");
  ksym_init();
  struct kstep_driver *driver = kstep_driver_get(kstep_params.driver);
  if (driver->pre_init)
    driver->pre_init();

  kstep_params_print();
  if (kstep_params.special_topo)
    kstep_use_special_topo();

  kstep_topo_print();
  kstep_patch_min_vruntime();

  // Isolate the CPUs to avoid interference
  kstep_prealloc_kworkers();
  kstep_disable_workqueue();
  kstep_move_kthreads();

  kstep_cgroup_init();

  // Run userspace programs when we know the system is ready
  if (driver->init)
    driver->init();

  // Control timer ticks and clock
  kstep_tick_init();

  // Enable printk time
  kstep_write_file("/sys/module/printk/parameters/time", "1", 1);

  // Reset the scheduler state to initial state
  kstep_reset_sched_state();

  if (kstep_params.print_lb_events)
    kstep_trace_lb();

  print_all_tasks();

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
