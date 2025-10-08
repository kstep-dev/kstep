#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "logging.h"

#define TARGET_CPU 1

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler trace");

// Invoked when `sched_tick` is called
static void sched_tick_callback(unsigned long ip, unsigned long parent_ip,
                                struct ftrace_ops *op,
                                struct ftrace_regs *fregs) {
  if (smp_processor_id() == TARGET_CPU)
    TRACE_INFO("sched_tick called on CPU %d\n", smp_processor_id());
}
static struct ftrace_ops ftrace_ops_sched_tick = {.func = &sched_tick_callback};

// Invoked when `update_rq_clock` is called
static void update_rq_clock_callback(unsigned long ip, unsigned long parent_ip,
                                     struct ftrace_ops *op,
                                     struct ftrace_regs *fregs) {
  if (smp_processor_id() == TARGET_CPU)
    TRACE_INFO("update_rq_clock called on CPU %d\n", smp_processor_id());
}
static struct ftrace_ops ftrace_ops_update_rq_clock = {
    .func = &update_rq_clock_callback};

static void __exit kmod_exit(void) {
  unregister_ftrace_function(&ftrace_ops_sched_tick);
  unregister_ftrace_function(&ftrace_ops_update_rq_clock);
}

static int __init kmod_init(void) {
  ftrace_set_filter(&ftrace_ops_sched_tick, "sched_tick", strlen("sched_tick"),
                    1);
  if (register_ftrace_function(&ftrace_ops_sched_tick)) {
    TRACE_ERROR("Failed to register ftrace_ops_sched_tick\n");
    goto err;
  }

  ftrace_set_filter(&ftrace_ops_update_rq_clock, "update_rq_clock",
                    strlen("update_rq_clock"), 1);
  if (register_ftrace_function(&ftrace_ops_update_rq_clock)) {
    TRACE_ERROR("Failed to register ftrace_ops_update_rq_clock\n");
    goto err;
  }

  TRACE_INFO("Ftrace initialized\n");
  return 0;

err:
  kmod_exit();
  return -EINVAL;
}

module_init(kmod_init);
module_exit(kmod_exit);
