#include <linux/module.h>

#include "kstep.h"

struct kstep_params_t kstep_params = {
    .controller = "noop",
    .step_interval_us = 19000ULL, // Cannot be larger than DELAY_CONST_MAX
    .special_topo = false,
    .print_tasks = true,
    .print_nr_running = false,
};
module_param_string(controller, kstep_params.controller,
                    sizeof(kstep_params.controller), 0644);
module_param_named(step_interval_us, kstep_params.step_interval_us, ullong,
                   0644);
module_param_named(special_topo, kstep_params.special_topo, bool, 0644);
module_param_named(print_tasks, kstep_params.print_tasks, bool, 0644);
module_param_named(print_nr_running, kstep_params.print_nr_running, bool, 0644);

void kstep_params_print(void) {
  TRACE_INFO("Kstep params:");
  TRACE_INFO("- controller: %s", kstep_params.controller);
  TRACE_INFO("- step_interval_us: %llu", kstep_params.step_interval_us);
  TRACE_INFO("- special_topo: %d", kstep_params.special_topo);
  TRACE_INFO("- print_tasks: %d", kstep_params.print_tasks);
  TRACE_INFO("- print_nr_running: %d", kstep_params.print_nr_running);
}

static struct task_struct *controller_task;

static int __init kmod_init(void) {
  ksym_init();
  struct controller_ops *ops = kstep_controller_get(kstep_params.controller);
  controller_task =
      kthread_create((void *)kstep_controller_run, ops, ops->name);
  if (IS_ERR(controller_task)) {
    panic("Failed to create controller task");
  }
  set_cpus_allowed_ptr(controller_task, cpumask_of(0));
  wake_up_process(controller_task);
  return 0;
}

static void __exit kmod_exit(void) { kthread_stop(controller_task); }

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");
