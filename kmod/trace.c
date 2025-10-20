#include <linux/ftrace.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mmu_context.h>
#include <linux/module.h>
#include <linux/sched/clock.h>

#include "logging.h"

// Linux private headers
#include <kernel/sched/sched.h>

#define FTRACE_FUNC_LIST                                                       \
  X(sched_tick)                                                                \
  X(update_rq_clock)

static void sched_tick_callback(unsigned long ip, unsigned long parent_ip,
                                struct ftrace_ops *op,
                                struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;
  TRACE_INFO("sched_tick called on CPU %d", smp_processor_id());
}

static void update_rq_clock_callback(unsigned long ip, unsigned long parent_ip,
                                     struct ftrace_ops *op,
                                     struct ftrace_regs *fregs) {
  if (smp_processor_id() == 0)
    return;

  struct rq *rq = (void *)ftrace_regs_get_argument(fregs, 0);
  u64 clock = sched_clock();
  s64 delta = (s64)clock - rq->clock;
  TRACE_INFO("update_rq_clock called on CPU %d, clock=%llu, rq->clock=%llu, "
             "delta=%lld",
             smp_processor_id(), clock, rq->clock, delta);
}

#define X(name)                                                                \
  static struct ftrace_ops ftrace_ops_##name = {.func = &name##_callback};
FTRACE_FUNC_LIST
#undef X

static void __exit kmod_exit(void) {
#define X(name) unregister_ftrace_function(&ftrace_ops_##name);
  FTRACE_FUNC_LIST
#undef X
}

static int __init kmod_init(void) {
#define X(name)                                                                \
  ftrace_set_filter(&ftrace_ops_##name, #name, strlen(#name), 1);              \
  if (register_ftrace_function(&ftrace_ops_##name)) {                          \
    TRACE_ERR("Failed to register ftrace_ops_##name");                         \
    goto err;                                                                  \
  }
  FTRACE_FUNC_LIST
#undef X

  TRACE_INFO("Ftrace initialized");
  return 0;

err:
  kmod_exit();
  return -EINVAL;
}

module_init(kmod_init);
module_exit(kmod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Shawn Zhong");
MODULE_DESCRIPTION("Scheduler trace");
