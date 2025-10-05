#include <linux/module.h>
#include <linux/workqueue.h>

#define KERNEL_SYMBOL_LIST X(bool, sched_cpu_dying, (int cpu))
#include "kernel_sym.h"
#include "logging.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Freeze CPU");

static int cpu;
module_param(cpu, int, 0644);
MODULE_PARM_DESC(cpu, "CPU to freeze");

static void handler(struct work_struct *work) {
  TRACE_INFO("Freezing CPU %d\n", cpu);
  kernel_sched_cpu_dying(cpu);
}

static int __init freeze_init(void) {
  init_kernel_symbols();

  static struct work_struct work;
  INIT_WORK(&work, handler);
  schedule_work_on(cpu, &work);

  return 0;
}

module_init(freeze_init);
