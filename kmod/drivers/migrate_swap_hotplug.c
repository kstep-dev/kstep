// https://github.com/torvalds/linux/commit/009836b4fa52
//
// Bug: ttwu_queue_cond() lacks a stop_sched_class check, so stopper thread
// wakeups from a remote CPU can be deferred through ttwu_wakelist when the
// target CPU is idle (nr_running == 0). This delays the stopper from becoming
// runnable during CPU hotplug, allowing a double balance_push.
//
// Test: Wake CPU 1's stopper thread from CPU 0 while CPU 1 is idle.
// Buggy kernel: wakeup deferred (state=TASK_WAKING after wake_up_process).
// Fixed kernel: wakeup immediate (state=TASK_RUNNING after wake_up_process).

#include "internal.h"
#include <linux/stop_machine.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 16, 0)

struct cpu_stopper {
  struct task_struct *thread;
  raw_spinlock_t lock;
  bool enabled;
  struct list_head works;
  struct cpu_stop_work stop_work;
  unsigned long caller;
  cpu_stop_fn_t fn;
};

KSYM_IMPORT_TYPED(struct cpu_stopper, cpu_stopper);

static void setup(void) {}

static void run(void) {
  struct cpu_stopper *stopper = per_cpu_ptr(KSYM_cpu_stopper, 1);
  struct task_struct *st = stopper->thread;
  struct rq *rq1 = cpu_rq(1);

  if (!st) {
    kstep_fail("CPU 1 stopper thread is NULL");
    return;
  }

  kstep_tick_repeat(5);

  TRACE_INFO("CPU1 stopper pid=%d state=0x%x on_rq=%d nr_running=%d",
             st->pid, READ_ONCE(st->__state), READ_ONCE(st->on_rq),
             READ_ONCE(rq1->nr_running));

  int waking_count = 0;
  int direct_count = 0;
  int test_count = 0;

  for (int i = 0; i < 10; i++) {
    unsigned int state_before = READ_ONCE(st->__state);

    if (!(state_before & (TASK_INTERRUPTIBLE | TASK_UNINTERRUPTIBLE))) {
      kstep_tick_repeat(3);
      continue;
    }

    int ret = wake_up_process(st);

    unsigned int state_after = READ_ONCE(st->__state);
    int on_rq_after = READ_ONCE(st->on_rq);

    if (ret) {
      test_count++;
      if (state_after == TASK_WAKING) {
        waking_count++;
        TRACE_INFO("[%d] DEFERRED: state=0x%x on_rq=%d", i, state_after,
                   on_rq_after);
      } else {
        direct_count++;
        TRACE_INFO("[%d] DIRECT: state=0x%x on_rq=%d", i, state_after,
                   on_rq_after);
      }
    }

    kstep_tick_repeat(3);
  }

  TRACE_INFO("tests=%d deferred=%d direct=%d", test_count, waking_count,
             direct_count);

  if (test_count == 0) {
    kstep_pass("could not test - stopper never sleeping");
  } else if (waking_count > 0) {
    kstep_fail("stopper wakeup deferred %d/%d times via ttwu_wakelist - "
               "ttwu_queue_cond missing stop_sched_class check",
               waking_count, test_count);
  } else {
    kstep_pass("stopper wakeup always direct (%d tests) - "
               "ttwu_queue_cond stop_sched_class check present",
               test_count);
  }
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "migrate_swap_hotplug",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
    .tick_interval_ns = 1000000,
};
