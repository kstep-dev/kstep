#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/smp.h>

#define KERNEL_SYMBOL_LIST                                                     \
  X(void, arch_smp_send_reschedule, (int cpu))                                 \
  X(void, native_smp_send_reschedule, (int cpu))                               \
  X(bool, sched_cpu_dying, (int cpu))
#include "kernel_sym.h"
#include "logging.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Send reschedule IPI");

static int __init resched_ipi_init(void) {
  init_kernel_symbols();
  int curr_cpu = smp_processor_id();
  if (curr_cpu != 0) {
    TRACE_ERROR("Current CPU is not 0, skipping\n");
    return 0;
  }

  int target_cpu = 1;
  TRACE_INFO("Sending reschedule IPI from CPU %d to CPU %d\n", curr_cpu,
             target_cpu);
  if (kernel_arch_smp_send_reschedule) {
    kernel_arch_smp_send_reschedule(target_cpu);
  } else if (kernel_native_smp_send_reschedule) {
    kernel_native_smp_send_reschedule(target_cpu);
  } else {
    TRACE_ERROR("Reschedule IPI not supported\n");
    return -ENOSYS;
  }
  return 0;
}

static void __exit resched_ipi_exit(void) {
  pr_info("Reschedule IPI module exit\n");
}

module_init(resched_ipi_init);
module_exit(resched_ipi_exit);
