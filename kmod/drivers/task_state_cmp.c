// https://github.com/torvalds/linux/commit/5aec788aeb8eb74282b75ac1b317beb0fbb69a42
//
// Bug: ___wait_is_interruptible(TASK_INTERRUPTIBLE|TASK_FREEZABLE) returns
// false due to equality comparison (state == TASK_INTERRUPTIBLE), causing
// wait_event_freezable() to ignore signals and spin forever.
//
// Fix: Use bitmask check (state & (TASK_INTERRUPTIBLE | TASK_WAKEKILL)).

#include "driver.h"
#include <linux/kthread.h>
#include <linux/version.h>
#include <linux/wait.h>

#if LINUX_VERSION_CODE == KERNEL_VERSION(5, 19, 0)

static struct task_struct *kt;
static DECLARE_WAIT_QUEUE_HEAD(test_wq);
static volatile int wq_flag;
static volatile int phase; // 0=spin, 1=entered_wait, 2=exited_wait

static int test_kthread_fn(void *data) {
  while (phase == 0)
    cpu_relax();

  // Enter wait_event_freezable with TIF_SIGPENDING already set.
  // On buggy kernel: ___wait_is_interruptible returns false, so the signal
  // never breaks the inner loop -> kthread busy-loops in TASK_RUNNING.
  // On fixed kernel: ___wait_is_interruptible returns true, signal breaks
  // the loop -> wait_event_freezable returns -ERESTARTSYS.
  wait_event_freezable(test_wq, wq_flag);
  phase = 2;

  while (!kthread_should_stop())
    cpu_relax();
  return 0;
}

static void setup(void) {
  wq_flag = 0;
  phase = 0;
  kt = kthread_create(test_kthread_fn, NULL, "state_cmp_test");
  if (IS_ERR(kt))
    panic("kthread_create failed");
  kthread_bind(kt, 1);
  wake_up_process(kt);
}

static void run(void) {
  kstep_tick_repeat(5);
  TRACE_INFO("kthread state before signal: 0x%x", READ_ONCE(kt->__state));

  // Set TIF_SIGPENDING directly on the kthread. This simulates the situation
  // where freeze_task() fails to directly freeze a running task and falls
  // through to fake_signal_wake_up.
  set_tsk_thread_flag(kt, TIF_SIGPENDING);

  // Tell kthread to enter wait_event_freezable
  phase = 1;
  kstep_tick_repeat(20);

  unsigned int state = READ_ONCE(kt->__state);
  TRACE_INFO("After wait entry: phase=%d state=0x%x", phase, state);

  if (phase == 2) {
    kstep_pass("wait_event_freezable correctly returns on signal");
  } else {
    kstep_fail("wait_event_freezable ignores signal, task stuck (state=0x%x)",
               state);
  }

  // Cleanup: let the kthread escape the buggy loop
  clear_tsk_thread_flag(kt, TIF_SIGPENDING);
  wq_flag = 1;
  wake_up(&test_wq);
  kstep_tick_repeat(5);
}

KSTEP_DRIVER_DEFINE{
    .name = "task_state_cmp",
    .setup = setup,
    .run = run,
    .step_interval_us = 10000,
};

#else

KSTEP_DRIVER_DEFINE{
    .name = "task_state_cmp",
};

#endif
