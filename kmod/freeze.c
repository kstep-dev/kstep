#include <linux/module.h>

#define KERNEL_SYMBOL_LIST X(bool, sched_cpu_dying, (int cpu))
#include "kernel_sym.h"
#include "logging.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Freeze CPU");

static int __init freeze_init(void) {
  init_kernel_symbols();
  kernel_sched_cpu_dying(1);
  TRACE_INFO("Freezing CPU %d\n", 1);
  return 0;
}

module_init(freeze_init);
