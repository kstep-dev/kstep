#include <linux/kthread.h>
#include <linux/module.h>

#include "controller.h"
#include "internal.h"
#include "ksym.h"
#include "logging.h"

static char controller_name[32] = "noop";
module_param_string(controller, controller_name, sizeof(controller_name), 0644);
MODULE_PARM_DESC(controller, "Controller name to run");

static struct task_struct *controller_task;

static struct controller_ops *get_controller_ops(const char *name) {
  for (int i = 0; i < ARRAY_SIZE(controller_ops_list); i++) {
    if (strcmp(controller_ops_list[i]->name, name) == 0) {
      return controller_ops_list[i];
    }
  }
  return NULL;
}

static int __init kmod_init(void) {
  ksym_init();
  struct controller_ops *ops = get_controller_ops(controller_name);
  if (ops == NULL) {
    TRACE_ERR("Controller %s not found", controller_name);
    return -EINVAL;
  }
  controller_task = kthread_create((void *)controller_run, ops, ops->name);
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
