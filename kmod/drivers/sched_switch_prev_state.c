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

#if LINUX_VERSION_CODE == KERNEL_VERSION(6, 14, 0)

static struct task_struct *victim;
static struct task_struct *spinner;

static volatile int phase; // 0=spin, 1=do_interruptible_schedule, 2=done

static volatile bool bug_detected;
static volatile unsigned int traced_prev_state;
static volatile unsigned int actual_state;
static volatile int probe_count;

// Tracepoint probe for sched_switch (4-param version in 6.14):
//   bool preempt, task_struct *prev, task_struct *next, unsigned int prev_state
static void probe_sched_switch(void *data, bool preempt,
                                struct task_struct *prev,
                                struct task_struct *next,
                                unsigned int prev_state) {
  if (prev != victim)
    return;

  probe_count++;
  traced_prev_state = prev_state;
  actual_state = READ_ONCE(prev->__state);

  // Bug: tracepoint reports sleeping state but task is actually TASK_RUNNING
  if (prev_state != 0 && actual_state == TASK_RUNNING)
    bug_detected = true;
}

static int victim_fn(void *data) {
  while (phase == 0)
    cpu_relax();

  // Directly set TASK_INTERRUPTIBLE and call schedule().
  // With TIF_SIGPENDING set by the driver, __schedule -> try_to_block_task
  // will find signal_pending_state() true, set __state = TASK_RUNNING, but
  // (on buggy kernel) not update the caller's prev_state variable.
  // Repeat multiple times to ensure at least one context switch occurs.
  for (int i = 0; i < 20; i++) {
    set_current_state(TASK_INTERRUPTIBLE);
    schedule();
  }

  phase = 2;
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
  phase = 0;
  bug_detected = false;
  traced_prev_state = 0;
  actual_state = 0;
  probe_count = 0;

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

  // Let both tasks run and accumulate some vruntime
  kstep_tick_repeat(5);

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

  // Set TIF_SIGPENDING so signal_pending_state() returns true inside
  // try_to_block_task when the victim calls schedule() with TASK_INTERRUPTIBLE
  set_tsk_thread_flag(victim, TIF_SIGPENDING);

  // Tell victim to start calling set_current_state + schedule
  phase = 1;

  // Run ticks to let the victim execute its schedule() loop
  kstep_tick_repeat(30);

  tracepoint_probe_unregister(tp, probe_sched_switch, NULL);
  tracepoint_synchronize_unregister();

  TRACE_INFO("probe_count=%d traced_prev_state=0x%x actual_state=0x%x "
             "bug_detected=%d phase=%d",
             probe_count, traced_prev_state, actual_state,
             bug_detected, phase);

  if (probe_count == 0) {
    kstep_fail("sched_switch probe never fired for victim");
  } else if (bug_detected) {
    kstep_fail("stale prev_state=0x%x in trace_sched_switch "
               "(actual __state=0x%x is TASK_RUNNING)",
               traced_prev_state, actual_state);
  } else {
    kstep_pass("trace_sched_switch correctly reports state=0x%x "
               "(probe_count=%d)",
               traced_prev_state, probe_count);
  }

  // Cleanup
  clear_tsk_thread_flag(victim, TIF_SIGPENDING);
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
