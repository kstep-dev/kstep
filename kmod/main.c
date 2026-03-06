#include <generated/utsrelease.h>
#include <linux/module.h>
#include <linux/reboot.h>

#include "driver.h"
#include "internal.h"

struct kstep_driver *kstep_driver = NULL;

static char driver_name[DRIVER_NAME_LEN] = "default";
module_param_string(driver, driver_name, DRIVER_NAME_LEN, 0644);

enum kstep_status {
  KSTEP_STATUS_PENDING,
  KSTEP_STATUS_PASS,
  KSTEP_STATUS_FAIL,
};
static enum kstep_status status = KSTEP_STATUS_PENDING;

void kstep_status_set_pass(void) {
  if (status == KSTEP_STATUS_FAIL)
    panic("pass after fail not allowed");
  status = KSTEP_STATUS_PASS;
}
void kstep_status_set_fail(void) { status = KSTEP_STATUS_FAIL; }

static int __init kstep_main(void) {
  kstep_output_init();

  TRACE_INFO("Linux version: %s", UTS_RELEASE);
  kstep_driver = kstep_sym_init(driver_name);
  kstep_driver_print(kstep_driver);

  // Isolate the CPUs to avoid interference
  kstep_prealloc_kworkers();
  kstep_disable_workqueue();
  kstep_move_kthreads();

  // Run userspace programs when we know the system is ready
  kstep_task_init();
  kstep_cgroup_init();
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
  kstep_driver->run();

  TRACE_INFO("Exiting driver %s on Linux %s", kstep_driver->name, UTS_RELEASE);

  if (status == KSTEP_STATUS_PENDING || status == KSTEP_STATUS_PASS)
    kstep_pass();
  else if (status == KSTEP_STATUS_FAIL)
    kstep_fail();

  kernel_restart(NULL);

  return 0;
}
module_init(kstep_main);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("kSTEP");
