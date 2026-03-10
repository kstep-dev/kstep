// https://github.com/torvalds/linux/commit/8feb053d5319
//
// Bug: try_to_block_task() takes task_state by value. When signal_pending_state()
// returns true, it sets p->__state = TASK_RUNNING but doesn't update the caller's
// prev_state local variable. trace_sched_switch() then sees a stale sleeping state.
//
// Fix: Change try_to_block_task() to take a pointer to task_state so it can
// propagate the TASK_RUNNING update back to the caller.

#include "internal.h"
#include <linux/kthread.h>
#include <linux/tracepoint.h>
#include <linux/version.h>
#include <linux/wait.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 14, 0)

static struct task_struct *victim;
static struct task_struct *spinner;

static DECLARE_WAIT_QUEUE_HEAD(test_wq);
static volatile int wq_flag;
static volatile int phase; // 0=spin, 1=enter_wait

static volatile bool probe_fired;
static volatile unsigned int traced_prev_state;
static volatile unsigned int actual_state;

// Tracepoint probe for sched_switch (4-param version in 6.14):
//   bool preempt, task_struct *prev, task_struct *next, unsigned int prev_state
static void probe_sched_switch(void *data, bool preempt,
                                struct task_struct *prev,
                                struct task_struct *next,
                                unsigned int prev_state) {
  if (prev != victim)
    return;

  traced_prev_state = prev_state;
  actual_state = READ_ONCE(prev->__state);
  probe_fired = true;
}

static int victim_fn(void *data) {
  while (phase == 0)
    cpu_relax();

  // Enter wait_event_interruptible with TIF_SIGPENDING already set.
  // This sets TASK_INTERRUPTIBLE, then calls schedule().
  // In __schedule -> try_to_block_task: signal_pending_state() returns true,
  // sets __state = TASK_RUNNING but (on buggy) doesn't update prev_state.
  wait_event_interruptible(test_wq, wq_flag);

  while (!kthread_should_stop())
    cpu_relax();
  return 0;
}

static int spinner_fn(void *data) {
  while (!kthread_should_stop())
    cpu_relax();
  return 0;
}

static void setup(void) {
  wq_flag = 0;
  phase = 0;
  probe_fired = false;
  traced_prev_state = 0;
  actual_state = 0;

  victim = kthread_create(victim_fn, NULL, "sw_victim");
  if (IS_ERR(victim))
    panic("kthread_create victim failed");
  kthread_bind(victim, 1);
  wake_up_process(victim);

  spinner = kthread_create(spinner_fn, NULL, "sw_spinner");
  if (IS_ERR(spinner))
    panic("kthread_create spinner failed");
  kthread_bind(spinner, 1);
  wake_up_process(spinner);
}

static void run(void) {
  struct tracepoint *tp;
  int ret;

  kstep_tick_repeat(5);

  // Register tracepoint probe via runtime symbol lookup
  tp = kstep_ksym_lookup("__tracepoint_sched_switch");
  if (!tp) {
    kstep_fail("cannot find __tracepoint_sched_switch symbol");
    return;
  }

  ret = tracepoint_probe_register(tp, probe_sched_switch, NULL);
  if (ret) {
    kstep_fail("tracepoint_probe_register failed: %d", ret);
    return;
  }

  TRACE_INFO("victim pid=%d state=0x%x",
             victim->pid, READ_ONCE(victim->__state));

  // Set TIF_SIGPENDING so signal_pending_state() returns true
  set_tsk_thread_flag(victim, TIF_SIGPENDING);

  // Tell victim to enter wait_event_interruptible
  phase = 1;

  kstep_tick_repeat(20);

  tracepoint_probe_unregister(tp, probe_sched_switch, NULL);
  tracepoint_synchronize_unregister();

  TRACE_INFO("probe_fired=%d traced_prev_state=0x%x actual_state=0x%x",
             probe_fired, traced_prev_state, actual_state);

  if (!probe_fired) {
    kstep_fail("sched_switch probe never fired for victim");
  } else if (traced_prev_state != 0 && actual_state == TASK_RUNNING) {
    // Bug: tracepoint saw sleeping state but task is actually TASK_RUNNING
    kstep_fail("stale prev_state=0x%x in trace_sched_switch "
               "(actual __state=0x%x is TASK_RUNNING)",
               traced_prev_state, actual_state);
  } else {
    kstep_pass("trace_sched_switch correctly reports state=0x%x",
               traced_prev_state);
  }

  // Cleanup
  clear_tsk_thread_flag(victim, TIF_SIGPENDING);
  wq_flag = 1;
  wake_up(&test_wq);
  kstep_tick_repeat(5);
}

#else
static void setup(void) {}
static void run(void) { kstep_pass("kernel version not applicable"); }
#endif

KSTEP_DRIVER_DEFINE{
    .name = "sched_switch_prev_state",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};
