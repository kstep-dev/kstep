#include <linux/delay.h>
#include <linux/ftrace.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#define KERNEL_SYMBOL_LIST                                                     \
  X(void, tick_sched_timer_dying, (int cpu))                                   \
  X(void, sched_tick, (void))                                                  \
  X(u64, sched_clock, (void))                                                  \
  X(void, paravirt_set_sched_clock, (u64(*func)(void)))                        \
  X(u64, kvm_sched_clock_read, (void))                                         \
  X(void, clear_sched_clock_stable, (void))

#include "kernel_sym.h"
#include "logging.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler control");

static u64 mocked_sched_clock_value = 0;
static u64 mocked_sched_clock(void) {
  if (smp_processor_id() == 0)
    return kernel_kvm_sched_clock_read();
  return mocked_sched_clock_value;
}

static void remote_fn(void *data) {
  TRACE_INFO("sched_tick initiated on CPU %d: comm=%s, clock=%llu\n",
             smp_processor_id(), current->comm, kernel_sched_clock());
  mocked_sched_clock_value += 1000000;
  kernel_sched_tick();
}

static int main_kthread(void *data) {
  while (1) {
    msleep(5000);
    smp_call_function_single(1, remote_fn, NULL, 0);
  }
  return 0;
}

static void sched_tick_callback(unsigned long ip, unsigned long parent_ip,
                                struct ftrace_ops *op,
                                struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;
  TRACE_INFO("sched_tick called on CPU %d\n", smp_processor_id());
}

static int __init init(void) {
  if (smp_processor_id() != 0) {
    TRACE_ERROR("Current CPU %d is not 0\n", smp_processor_id());
    return -EINVAL;
  }
  init_kernel_symbols();

  TRACE_INFO("Freezing CPU %d\n", 1);
  kernel_tick_sched_timer_dying(1);
  kernel_clear_sched_clock_stable();

  mocked_sched_clock_value = kernel_kvm_sched_clock_read() / 1000000 * 1000000;
  kernel_paravirt_set_sched_clock(mocked_sched_clock);

  // Initialize ftrace
  struct ftrace_ops ftrace_ops = {.func = &sched_tick_callback};
  ftrace_set_filter(&ftrace_ops, "sched_tick", strlen("sched_tick"), 1);
  if (register_ftrace_function(&ftrace_ops)) {
    TRACE_ERROR("Failed to initialize ftrace\n");
    return -EINVAL;
  }

  // Create kthread
  struct task_struct *task = kthread_run(main_kthread, NULL, "freeze");
  if (IS_ERR(task)) {
    TRACE_ERROR("Failed to create kthread\n");
    return PTR_ERR(task);
  }

  return 0;
}

module_init(init);
